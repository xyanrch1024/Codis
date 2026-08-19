#include "agent_loop.h"
#include "acp.h"
#include "context_utils.h"
#include "log.h"
#include "str_util.h"

#include <chrono>
#include <ctime>
#include <thread>

namespace codis {

namespace {


constexpr const char* kCompactPrompt =
    "You are summarizing a conversation to preserve context for continuing work later.\n"
    "\n"
    "**Critical**: This summary will be the ONLY context available when the conversation "
    "resumes. Assume all previous messages will be lost. Be thorough.\n"
    "\n"
    "**Required sections**:\n"
    "\n"
    "## Current State\n"
    "\n"
    "- What task is being worked on (exact user request)\n"
    "- Current progress and what's been completed\n"
    "- What's being worked on right now (incomplete work)\n"
    "- What remains to be done (specific next steps, not vague)\n"
    "\n"
    "## Files & Changes\n"
    "\n"
    "- Files that were modified (with brief description of changes)\n"
    "- Files that were read/analyzed (why they're relevant)\n"
    "- Key files not yet touched but will need changes\n"
    "- File paths and line numbers for important code locations\n"
    "\n"
    "## Technical Context\n"
    "\n"
    "- Architecture decisions made and why\n"
    "- Patterns being followed (with examples)\n"
    "- Libraries/frameworks being used\n"
    "- Commands that worked (exact commands with context)\n"
    "- Commands that failed (what was tried and why it didn't work)\n"
    "- Environment details (language versions, dependencies, etc.)\n"
    "\n"
    "## Strategy & Approach\n"
    "\n"
    "- Overall approach being taken\n"
    "- Why this approach was chosen over alternatives\n"
    "- Key insights or gotchas discovered\n"
    "- Assumptions made\n"
    "- Any blockers or risks identified\n"
    "\n"
    "## Exact Next Steps\n"
    "\n"
    "Be specific. Don't write \"implement authentication\" - write:\n"
    "\n"
    "1. Add JWT middleware to src/middleware/auth.js:15\n"
    "2. Update login handler in src/routes/user.js:45 to return token\n"
    "3. Test with: npm test -- auth.test.js\n"
    "\n"
    "**Tone**: Write as if briefing a teammate taking over mid-task. Include everything "
    "they'd need to continue without asking questions. No emojis ever.\n"
    "\n"
    "**Length**: No limit. Err on the side of too much detail rather than too little. "
    "Critical context is worth the tokens.\n"
    "\n"
    "Write the summary in the same language the user used. "
    "Output only the summary itself, no preamble or explanation.";

} // namespace

AgentLoop::AgentLoop(SessionHub& hub, SessionStore& store, SystemContext& system,
                     ToolRegistry& tools, ProviderRegistry& providers, const AppConfig& config)
    : hub_(hub), store_(store), system_(system), tools_(tools), providers_(providers),
      config_(config) {}

// =============================================================================
// run_task — 顶层异常防护 + 排队请求迭代补跑
// run_task_inner 抛出异常（数据库损坏、Provider 返回畸形 JSON 等）时不能让
// detached 线程终止进程；排队请求用循环补跑而不是递归，避免刷帧导致栈溢出。
// =============================================================================

void AgentLoop::run_task(const std::string& session_id, const std::string& conn_id,
                         ChatRequest req) {
    for (;;) {
        try {
            run_task_inner(session_id, conn_id, std::move(req));
        } catch (const std::exception& e) {
            LOG_ERROR("ACP task crashed, session {}: {}", session_id.substr(0, 8), e.what());
            hub_.broadcast(session_id, conn_id, acp::error_frame(std::string("internal error: ") + e.what()));
            hub_.broadcast(session_id, conn_id, acp::done_frame());
        } catch (...) {
            LOG_ERROR("ACP task crashed, session {}: unknown exception", session_id.substr(0, 8));
            hub_.broadcast(session_id, conn_id, acp::error_frame("internal error: unknown exception"));
            hub_.broadcast(session_id, conn_id, acp::done_frame());
        }
        // 有排队请求则保持 Running 相位，在本线程内迭代补跑（不递归、不新建线程）
        auto next = hub_.next_task_or_idle(session_id);
        if (!next) break;
        LOG_DEBUG("session {} rerun for queued message", session_id.substr(0, 8));
        req = std::move(*next);
    }
    LOG_DEBUG("session {} completed", session_id.substr(0, 8));
}

// =============================================================================
// run_task_inner — ACP 多轮循环（单次任务）
// =============================================================================

void AgentLoop::run_task_inner(const std::string& session_id, const std::string& conn_id,
                               ChatRequest req) {
    int empty_retries = 0;
    int malformed_retries = 0;

    LOG_DEBUG("ACP loop started, session {} conn={}", session_id.substr(0, 8), conn_id);

    json tools = json::array();
    for (auto& s : tools_.all_schemas()) {
        tools.push_back({{"type", "function"}, {"function", {
            {"name", s.name}, {"description", s.description},
            {"parameters", s.parameters}
        }}});
    }
    req.tools = tools;

    auto baseline = system_.build_baseline(session_id, store_);
    // store 历史已含调用方刚 append 的当前 user 消息，整体重放：
    // 顺序 = system baseline + 历史正序。重放条件见 context_utils::is_replayable：
    // system（压缩摘要）+ user + tool + 有效 assistant（带 tool_call_id 的中转
    // 消息或无空白正文）。reasoning 思维链本身不入上下文，但会挂回同轮 assistant
    // 消息（严格 thinking provider 要求回传 reasoning_content）。跨轮重放悬浮
    // tool_call 会导致 OpenAI 格式断链（严格 provider 报错/模型困惑），故过滤同轮往返。
    auto history = store_.load_messages(session_id);
    std::vector<Message> msgs;
    msgs.push_back({"system", baseline});
    auto replay = context_utils::replayable_messages(history);
    for (auto& m : replay) msgs.push_back(m);
    req.messages = std::move(msgs);

    std::string assistant_content;
    int turn = 0;
    bool is_first_turn = true;

    // 取消标志：从会话状态取共享指针，保证跨线程安全；任务开始时清掉旧标记
    auto cancel_flag = hub_.cancel_token(session_id);
    cancel_flag->store(false);

    auto is_canceled = [&] { return cancel_flag->load(); };

    while (turn < max_turns_) {
        turn++;
        LOG_DEBUG("ACP loop turn {}/{}", turn, max_turns_);

        if (is_canceled()) {
            hub_.broadcast(session_id, conn_id, acp::error_frame("canceled"));
            break;
        }

        if (!is_first_turn) {
            auto update = system_.reconcile(session_id, store_);
            if (update) req.messages.push_back({"system", *update});
        }
        is_first_turn = false;

        assistant_content.clear();
        auto prov = resolve_provider(req);
        if (!prov) { hub_.broadcast(session_id, conn_id, acp::error_frame("No provider")); break; }

        auto t0 = std::chrono::steady_clock::now();
        // 向 LLM 发 POST 前：推送 context 统计（当前估算 tokens / 配置最大窗口），
        // 由服务端计算、客户端仅展示
        hub_.broadcast(session_id, conn_id,
                       acp::context_stats_frame(context_utils::est_tokens(req.messages),
                                                provider_max_context(config_, providers_, prov->name())));
        auto llm_result = prov->stream_chat(
            req,
            [&](std::string_view delta) {
                assistant_content += delta;
                // tool_calls JSON 不作为文本广播，客户端改走 tool_call 帧
                if (delta.find("\"tool_calls\"") != std::string_view::npos) return;
                hub_.broadcast(session_id, conn_id, acp::assistant_frame(delta));
            },
            [&](std::string_view delta) {
                // 空 reasoning delta（混合思考模型的非思考请求会发 ""）：
                // 直接透传会让客户端渲染出只有标签的空思维链块
                if (delta.empty()) {
                    LOG_TRACE("session {} reasoning empty delta skipped", session_id.substr(0, 8));
                    return;
                }
                hub_.broadcast(session_id, conn_id, acp::reasoning_frame(delta));
            },
            cancel_flag.get());

        auto llm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        // 客户端取消：LLM 流被中断，结束本轮，不执行工具
        if (llm_result.error_code == LlmErrorCode::Canceled || is_canceled()) {
            LOG_INFO("session {} task canceled after {}ms", session_id.substr(0, 8), llm_ms);
            hub_.broadcast(session_id, conn_id, acp::done_frame());
            break;
        }

        if (!llm_result.success) {
            LOG_ERROR("LLM call failed after {}ms: {}", llm_ms, llm_result.error);
            hub_.broadcast(session_id, conn_id, acp::error_frame("LLM 调用失败: " + llm_result.error));
            break;
        }

        if (!llm_result.reasoning_content.empty())
            LOG_DEBUG("turn {} reasoning_content ({} bytes):\n{}", turn,
                      llm_result.reasoning_content.size(), llm_result.reasoning_content);

        LOG_DEBUG("turn {} LLM: {}ms {} tokens: {}", turn, llm_ms,
                  assistant_content.size(), assistant_content.substr(0, 200));
        LOG_DEBUG("turn {} LLM full output ({} bytes):\n{}", turn,
                  assistant_content.size(), assistant_content);

        // 持久化思维链（独立行，恢复会话时按顺序在 assistant 前展示；不进入模型上下文）
        if (!llm_result.reasoning_content.empty())
            store_.append_message(session_id, {"reasoning", llm_result.reasoning_content});

        // 存历史时剥掉内嵌的 tool_calls JSON（只存可见文本），避免恢复会话时
        // 把原始 JSON 当正文展示给用户/模型
        std::string assistant_text = assistant_content;
        {
            auto [jbegin, jend] = context_utils::tool_calls_json_span(assistant_content);
            if (jbegin != std::string::npos && jbegin < jend)
                assistant_text = assistant_content.substr(0, jbegin) + assistant_content.substr(jend);
        }
        // 剥完 JSON 后若只剩空白（模型输出只用 tool_calls JSON 时残留换行），
        // 不入库，避免空白 assistant 条目污染展示与重放上下文
        if (!assistant_text.empty() && !context_utils::is_blank(assistant_text))
            store_.append_message(session_id, {"assistant", assistant_text});

        auto call_list = context_utils::extract_tool_calls(assistant_content);
        if (call_list.empty()) {
            // 空响应保护：模型只回思维链或直接停（GLM thinking 耗尽 max_tokens 时 content 为空）
            // 重试一次，仍空则明确报错，避免"无输出就完成"
            if (assistant_content.empty()) {
                if (empty_retries < kMaxEmptyRetries) {
                    empty_retries++;
                    LOG_WARN("LLM returned empty response (reasoning {} bytes), retry {}/{}",
                             llm_result.reasoning_content.size(), empty_retries, kMaxEmptyRetries);
                    continue;
                }
                hub_.broadcast(session_id, conn_id, acp::error_frame(
                    "模型返回空响应（可能思维链耗尽 max_tokens）。reasoning_content 大小: " +
                    std::to_string(llm_result.reasoning_content.size()) + " bytes"));
            } else if (assistant_content.find("\"tool_calls\"") != std::string::npos) {
                // 模型尝试调用工具但 JSON 解析失败：提示重试，而不是静默结束任务
                if (malformed_retries < kMaxMalformedRetries) {
                    malformed_retries++;
                    LOG_WARN("turn {} malformed tool_calls JSON, asking model to retry ({}/{})",
                             turn, malformed_retries, kMaxMalformedRetries);
                    req.messages.push_back({"system",
                        "你上一条回复的 tool_calls JSON 格式错误（括号不匹配或含非法字符），无法执行。"
                        "请重新输出语法正确的 tool_calls JSON，不要附加多余文本。"});
                    continue;
                }
                hub_.broadcast(session_id, conn_id,
                               acp::error_frame("模型连续返回格式错误的 tool_calls JSON"));
            }
            break;
        }

        for (auto& call : call_list) {
            auto perm = tools_.check_permission(call.name);
            if (perm == Permission::Denied) {
                hub_.broadcast(session_id, conn_id, acp::tool_call_frame(call.id, call.name, call.arguments));
                hub_.broadcast(session_id, conn_id,
                               acp::tool_result_frame(call.id, false, "Permission denied"));
                continue;
            }
            if (perm == Permission::Ask) {
                // Ask 权限：先征询用户确认，批准后才广播 tool_call 帧并执行
                auto broadcast = [&](const std::string& frame) {
                    hub_.broadcast(session_id, conn_id, frame);
                };
                if (!wait_for_confirmation(session_id, call, broadcast, cancel_flag)) {
                    hub_.broadcast(session_id, conn_id, acp::tool_result_frame(call.id, false,
                        is_canceled() ? "Canceled while awaiting confirmation"
                                      : "Tool call rejected (confirmation declined or timed out)"));
                    if (is_canceled()) break;  // 任务被取消，结束整个 ACP 循环
                    continue;
                }
            }
            hub_.broadcast(session_id, conn_id, acp::tool_call_frame(call.id, call.name, call.arguments));
            auto result = tools_.execute(call);
            hub_.broadcast(session_id, conn_id, acp::tool_result_frame(result.id, result.success, result.content));

            Message asst_msg; asst_msg.role = "assistant";
            asst_msg.tool_call_id = call.id; asst_msg.tool_name = call.name;
            asst_msg.tool_arguments = call.arguments;
            if (!llm_result.reasoning_content.empty())
                asst_msg.reasoning_content = llm_result.reasoning_content;
            req.messages.push_back(asst_msg);
            store_.append_message(session_id, asst_msg);

            Message tool_msg; tool_msg.role = "tool";
            tool_msg.content = result.content; tool_msg.tool_call_id = call.id;
            req.messages.push_back(tool_msg);
            store_.append_message(session_id, tool_msg);
        }
    }

    if (turn >= max_turns_)
        hub_.broadcast(session_id, conn_id, acp::error_frame("Max turns reached"));

    hub_.broadcast(session_id, conn_id, acp::done_frame());
}

// =============================================================================
// wait_for_confirmation — Ask 权限工具的执行前确认
// 广播 tool_confirm 帧给 session 的所有连接，挂起等待任一连接的 confirm_ack。
// 超时（config [permissions].confirm_timeout）视为拒绝；任务被取消立即返回 false。
// =============================================================================

bool AgentLoop::wait_for_confirmation(const std::string& session_id, const ToolCall& call,
                                      const std::function<void(const std::string&)>& broadcast,
                                      const std::shared_ptr<std::atomic<bool>>& cancel_flag) {
    std::string confirm_id = util::gen_short_id();
    auto slot = hub_.add_confirm(session_id, confirm_id);
    slot->call = call;

    LOG_INFO("session {} awaiting confirmation for tool '{}' ({})",
             session_id.substr(0, 8), call.name, confirm_id);
    broadcast(acp::tool_confirm_frame(confirm_id, call.id, call.name, call.arguments,
                                      config_.permissions.confirm_timeout_seconds));

    int timeout_s = config_.permissions.confirm_timeout_seconds;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    bool canceled = false;
    {
        std::unique_lock lk(slot->mutex);
        while (!slot->answered) {
            if (cancel_flag && cancel_flag->load()) { canceled = true; break; }
            if (std::chrono::steady_clock::now() >= deadline) break;
            // 分段等待：200ms 粒度轮询取消标记，避免取消后空等整个超时
            slot->cv.wait_until(
                lk, std::chrono::steady_clock::now() + std::chrono::milliseconds(200));
        }
    }

    bool approved = slot->answered && slot->approved;
    hub_.erase_confirm(session_id, confirm_id);

    LOG_INFO("session {} confirmation for tool '{}': {}", session_id.substr(0, 8), call.name,
             canceled ? "canceled"
                      : (slot->answered ? (approved ? "approved" : "rejected") : "timed out"));
    return approved;
}

// =============================================================================
// run_compact — 头部历史 → LLM 摘要 + 保留尾部窗口，写回 session_store
// =============================================================================

std::optional<ChatRequest> AgentLoop::run_compact(const std::string& session_id, int keep) {
    keep = context_utils::clamp_keep(keep);

    auto broadcast = [&](const std::string& frame) {
        hub_.broadcast(session_id, "", frame);
    };
    auto fail = [&](const std::string& why) {
        LOG_WARN("compact failed, session {}: {}", session_id.substr(0, 8), why);
        broadcast(acp::compacted_frame(false, "", why));
    };

    // 任务执行中（LLM 循环/其它压缩）拒绝并发，避免 SQLite 读写竞争
    if (!hub_.start_compact(session_id)) {
        fail("task in progress, retry after it finishes");
        return std::nullopt;
    }

    // 压缩开始前快照，留作后续 /restore 恢复位（本期只写不读）
    try {
        auto history = store_.load_messages(session_id);
        const size_t n = history.size();
        auto split = context_utils::split_history(history, keep);
        if (!split) {
            fail("history too short (" + std::to_string(n) + " msgs, need > " +
                 std::to_string(keep) + " to compress)");
            // 失败路径同样恢复 Idle 并排空 pending（原实现 processing 残留导致会话卡死）
            return hub_.finish_compact(session_id);
        }
        auto& prefix = split->prefix;
        auto& tail = split->tail;

        json snap = json::array();
        for (auto& m : history) snap.push_back(m.to_json());
        store_.save_context_snapshot(
            session_id, "compact:" + std::to_string(time(nullptr)), snap,
            "compacted " + std::to_string(n) + " msgs -> " +
            std::to_string(keep + 1) + " msgs");

        // LLM 摘要：专用 system 指令 + 头部历史，无工具，限 token。
        // 过滤 reasoning 角色（思维链）：openai 兼容端点对未知角色直接 400
        // （run_task_inner 重放路径同样过滤，此处与其保持一致）
        ChatRequest req;
        req.max_tokens = 1200;
        req.messages.push_back({"system", kCompactPrompt});
        std::string pending;
        for (auto& m : prefix)
            context_utils::append_with_reasoning(req.messages, m, pending);
        std::string summary = call_llm(req);

        // 新历史 = 摘要(system) + 题干(首条 user) + 尾部原文
        std::vector<Message> compacted = context_utils::build_compacted_history(*split, summary);

        store_.replace_messages(session_id, compacted);

        auto before_tok = context_utils::est_tokens(history);
        auto after_tok = context_utils::est_tokens(compacted);
        LOG_INFO("compacted session {}: {} msgs -> {} msgs ({} -> {} est tokens)",
                 session_id.substr(0, 8), n, compacted.size(), before_tok, after_tok);
        broadcast(acp::compacted_frame(true, summary, ""));
    } catch (const std::exception& e) {
        fail(e.what());
    }

    // 压缩结束：恢复 Idle；压缩期间排队的消息取第一个补跑（剩余由 run_task 迭代清空）
    return hub_.finish_compact(session_id);
}

// =============================================================================
// Provider 解析 + LLM 调用
// =============================================================================

std::shared_ptr<LLMProvider> AgentLoop::resolve_provider(const ChatRequest& req) {
    std::string name = req.provider.empty() ? providers_.default_name() : req.provider;
    LOG_DEBUG("resolving provider '{}'", name);
    auto provider = providers_.get(name);
    if (provider) return *provider;

    LOG_WARN("provider '{}' not found, fallback", name);
    auto list = providers_.list();
    if (!list.empty()) return *providers_.get(list[0]);

    LOG_ERROR("no provider configured");
    return nullptr;
}

std::string AgentLoop::call_llm(const ChatRequest& req) {
    auto prov = resolve_provider(req);
    if (!prov) throw std::runtime_error("No provider configured. Set API key env var (e.g. GLM_API_KEY)");
    LOG_INFO("LLM call: provider='{}' model='{}' (req.model='{}')",
             prov->name(), prov->get_model(), req.model);
    auto result = prov->chat(req);
    if (!result.success) {
        LOG_ERROR("LLM call failed: {}", result.error);
        throw std::runtime_error(result.error);
    }
    return result.content;
}

} // namespace codis
