#include "controller.h"

#include "log.h"
#include "tool_format.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace codis {

ChatController::ChatController(AcpClient& acp, std::shared_ptr<TuiState> state,
                               std::string model, std::string provider,
                               bool auto_approve, int server_port)
    : acp_(acp)
    , state_(std::move(state))
    , model_(std::move(model))
    , provider_(std::move(provider))
    , auto_approve_(auto_approve)
    , server_port_(server_port)
{
}

void ChatController::set_model_provider(std::string model, std::string provider) {
    model_ = std::move(model);
    provider_ = std::move(provider);
    state_->model = model_;
}

void ChatController::send_message(const std::string& text) {
    if (text == "/exit") {
        if (cb_.exit) cb_.exit();
        return;
    }
    if (text == "/clear") {
        cmd_clear();
        return;
    }
    if (text == "/sessions") {
        open_sessions();
        return;
    }
    if (text == "/info") {
        auto info = acp_.get_server_info();
        std::vector<SkillBrief> sk;
        std::vector<McpServerBrief> mcp;
        if (info) {
            sk = std::move(info->skills);
            mcp = std::move(info->mcp_servers);
        } else {
            state_->add_item(ItemKind::Error, "[Error] Server unreachable: " +
                std::string("failed to fetch /api/v1/info"));
        }
        if (cb_.show_info) cb_.show_info(std::move(sk), std::move(mcp));
        return;
    }
    if (text == "/clearsessions") {
        cmd_delete_all();
        return;
    }
    if (text == "/help") {
        if (cb_.show_help) cb_.show_help();
        return;
    }
    if (text.starts_with("/newsession")) {
        auto sid = acp_.create_session();
        if (sid) {
            state_->clear_all();
            state_->current_session = *sid;
            acp_.switch_session(*sid);  // WS 移到新 session，否则服务端广播无连接可发
            state_->add_item(ItemKind::Status, "[New session created: " + *sid + "]");
        }
        return;
    }
    if (text.starts_with("/balance")) {
        cmd_balance(text);
        return;
    }
    if (text.starts_with("/model")) {
        cmd_model(text);
        return;
    }
    if (text.starts_with("/yolo")) {
        // YOLO 模式热切换：所有 Ask 工具自动批准，不再弹确认
        std::string arg;
        if (text.size() > 5) arg = text.substr(5);
        for (char& c : arg) c = (char)std::tolower(c);
        if (arg == " on")       yolo_ = true;
        else if (arg == " off") yolo_ = false;
        else                    yolo_ = !yolo_;
        if (cb_.notice) cb_.notice(yolo_ ? "[YOLO mode ON — Ask 工具自动批准]"
                                         : "[YOLO mode OFF]");
        return;
    }
    if (text == "/compact" || text.starts_with("/compact ")) {
        cmd_compact(text);
        return;
    }

    // 当前任务处理中：新消息仅入 pending 队列，任务完成后自动发送
    if (state_->processing) {
        state_->pending_queue.push_back(text);
        if (cb_.reset_scroll) cb_.reset_scroll();
        return;
    }
    send_request(text);
}

void ChatController::send_request(const std::string& text) {
    state_->add_item(ItemKind::User, text);
    state_->processing = true;
    state_->request_start_ = std::chrono::steady_clock::now();
    state_->current_model_ = model_;
    if (cb_.reset_scroll) cb_.reset_scroll();

    state_->history.push_back({"user", text});

    // 上下文由服务端从 SQLite 重建（session 历史 + baseline + tools），
    // 客户端只发当前这一条 user 消息，整段 history 不再重复传输。
    std::vector<Message> msgs;
    msgs.push_back({"user", text});

    ChatRequest req;
    req.model = model_;
    req.provider = provider_;
    req.messages = msgs;
    req.max_tokens = 4096;
    req.session_id = state_->current_session;

    acp_.send_async(req);
}

void ChatController::flush_pending() {
    if (state_->processing) return;
    if (state_->pending_queue.empty()) return;
    std::string text = std::move(state_->pending_queue.front());
    state_->pending_queue.pop_front();
    send_request(text);
}

void ChatController::switch_session(const SessionInfo& s) {
    state_->current_session = s.id;
    if (cb_.hide_sessions) cb_.hide_sessions();

    // 切换到新 session 的 SSE（历史通过 REST 加载）
    acp_.switch_session(s.id);

    // 通过 REST API 拉历史
    auto info = acp_.get_session(s.id);
    state_->clear_all();
    if (info) load_history(info->messages);
    if (cb_.reset_scroll) cb_.reset_scroll();
    state_->add_item(ItemKind::Status, "[Session: " + s.id + "]");
    if (cb_.notify) cb_.notify();
}

void ChatController::delete_session(const SessionInfo& s) {
    bool was_current = (s.id == state_->current_session);
    acp_.delete_session(s.id);
    auto list = acp_.list_sessions();
    if (cb_.show_sessions) cb_.show_sessions(list, false);  // 保留当前选中（越界由视图层 clamp）
    if (list.empty()) {
        if (cb_.hide_sessions) cb_.hide_sessions();
        auto sid = acp_.create_session();
        if (sid) {
            state_->clear_all();
            state_->current_session = *sid;
        }
    } else if (was_current) {
        auto sid = acp_.create_session();
        if (sid) {
            state_->clear_all();
            state_->current_session = *sid;
            acp_.switch_session(*sid);  // WS 移到新 session
            state_->add_item(ItemKind::Status,
                "[Session " + s.id + " deleted, new session created]");
        }
    }
}

void ChatController::open_sessions() {
    if (cb_.show_sessions) cb_.show_sessions(acp_.list_sessions(), true);
}

void ChatController::cancel_task() {
    acp_.cancel_session(state_->current_session);
    state_->processing = false;
    state_->clear_pending();  // 取消时丢弃未发送的排队消息
    if (cb_.notice) cb_.notice("[Task cancelled]");
}

void ChatController::cmd_clear() {
    state_->clear_all();
    if (cb_.notify) cb_.notify();
}

void ChatController::cmd_compact(const std::string& line) {
    if (state_->processing) {
        state_->add_item(ItemKind::Status, "[任务执行中，结束后再 /compact 压缩]");
        if (cb_.notify) cb_.notify();
        return;
    }
    int keep = 20;
    auto pos = line.find(' ');
    if (pos != std::string::npos) {
        try { keep = std::stoi(line.substr(pos + 1)); } catch (...) {}
        keep = std::clamp(keep, 4, 100);
    }
    state_->add_item(ItemKind::Status,
        "[上下文压缩中…（LLM 摘要，约需数秒）keep=" + std::to_string(keep) + "]");
    acp_.send_compact(state_->current_session, keep);
    if (cb_.notify) cb_.notify();
}

void ChatController::cmd_delete_all() {
    acp_.delete_all_sessions();
    cmd_clear();
    state_->add_item(ItemKind::Status, "[All sessions deleted]");
    if (cb_.notify) cb_.notify();
}

void ChatController::cmd_balance(const std::string& line) {
    std::string prov = "deepseek";
    auto parts = [&]() {
        std::vector<std::string> v;
        std::istringstream iss(line);
        std::string w;
        while (iss >> w) v.push_back(w);
        return v;
    }();
    if (parts.size() > 1) prov = parts[1];

    auto http_res = acp_.http_get("/api/v1/balance/" + prov);
    if (!http_res.ok) {
        state_->add_item(ItemKind::Status,
            "[Error] Server unreachable: " + http_res.error);
        return;
    }
    if (http_res.status != 200) {
        try {
            auto j = codis::json::parse(http_res.body);
            state_->add_item(ItemKind::Status, "[Error] " + j.value("error", http_res.body));
        } catch (...) {
            state_->add_item(ItemKind::Status, "[Error] HTTP " +
                std::to_string(http_res.status) + ": " +
                http_res.body.substr(0, 200));
        }
        return;
    }

    try {
        auto j = codis::json::parse(http_res.body);
        auto& bal = j["balance"];
        state_->add_item(ItemKind::Status, "--- " + prov + " Balance ---");

        if (bal.contains("balance_infos") && !bal["balance_infos"].empty()) {
            for (auto& bi : bal["balance_infos"]) {
                state_->add_item(ItemKind::Status, "  Total:   " + bi.value("total_balance", "N/A"));
                state_->add_item(ItemKind::Status, "  Topped:  " + bi.value("topped_up_balance", "N/A"));
                state_->add_item(ItemKind::Status, "  Granted: " + bi.value("granted_balance", "N/A"));
            }
        } else {
            state_->add_item(ItemKind::Status, "  Response: " + bal.dump(2));
        }

        if (bal.contains("is_available")) {
            state_->add_item(ItemKind::Status, "  Active:  " + std::string(bal["is_available"].get<bool>() ? "Yes" : "No"));
        }
    } catch (const std::exception& e) {
        state_->add_item(ItemKind::Status, "[Error] Parse failed: " + std::string(e.what()));
    }
}

void ChatController::cmd_model(const std::string& line) {
    auto parts = [&]() {
        std::vector<std::string> v;
        std::istringstream iss(line);
        std::string w;
        while (iss >> w) v.push_back(w);
        return v;
    }();

    auto info = acp_.get_server_info();
    if (!info) {
        state_->add_item(ItemKind::Status, "[Error] Cannot query server info (unreachable?)");
        if (cb_.notify) cb_.notify();
        return;
    }

    // /model <name> — 切换到指定 provider（使用其配置的 model）
    if (parts.size() >= 2) {
        const std::string& name = parts[1];
        auto it = std::find(info->providers.begin(), info->providers.end(), name);
        if (it == info->providers.end()) {
            state_->add_item(ItemKind::Status, "[Error] Unknown provider: " + name);
            if (cb_.notify) cb_.notify();
            return;
        }
        provider_ = name;
        auto mit = info->provider_models.find(name);
        if (mit != info->provider_models.end()) {
            model_ = mit->second;
            state_->model = model_;
        }
        state_->add_item(ItemKind::Status, "[Model switched to " + name +
                          (mit != info->provider_models.end() ? " (" + mit->second + ")" : "") + "]");
        if (cb_.notify) cb_.notify();
        return;
    }

    // /model — 列出当前 + 可用 provider
    state_->add_item(ItemKind::Status, "--- Model ---");
    state_->add_item(ItemKind::Status, "  Current: " + provider_ + " (" + model_ + ")");
    for (auto& p : info->providers) {
        auto mit = info->provider_models.find(p);
        std::string model = mit != info->provider_models.end() ? mit->second : "?";
        state_->add_item(ItemKind::Status, "  " + p + " → " + model +
                          (p == provider_ ? "  (current)" : "") +
                          (p == info->default_provider ? "  (default)" : ""));
    }
    state_->add_item(ItemKind::Status, "  Usage: /model <provider>");
    if (cb_.notify) cb_.notify();
}

AcpClient::Callbacks ChatController::build_callbacks() {
    return {
        .on_assistant = [this](std::string_view delta) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::AssistantDelta;
            ev.text = std::string(delta);
            state_->push_event(ev);
        },
        .on_reasoning = [this](std::string_view delta) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::ReasoningDelta;
            ev.text = std::string(delta);
            state_->push_event(ev);
        },
        .on_tool_call = [this](const acp::ToolCallEvent& tc) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::ToolCall;
            ev.tool_call = tc;
            state_->push_event(ev);
        },
        .on_tool_result = [this](const acp::ToolResultEvent& tr) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::ToolResult;
            ev.tool_result = tr;
            state_->push_event(ev);
        },
        .on_tool_confirm = [this](const std::string& confirm_id, const acp::ToolCallEvent& tc,
                              int timeout_seconds) {
            if (auto_approve_ || yolo_) {
                // -y 或 /yolo 模式：静默批准，不打扰交互
                acp_.send_confirmation(confirm_id, true);
                LOG_INFO("auto-approved tool '{}' ({})", tc.name, confirm_id);
                return;
            }
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::ToolConfirm;
            ev.confirm_id = confirm_id;
            ev.tool_call = tc;
            ev.timeout_seconds = timeout_seconds;
            state_->push_event(ev);
        },
        .on_error = [this](std::string_view msg) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::Error;
            ev.text = std::string(msg);
            state_->push_event(ev);
        },
        .on_done = [this]() {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::Done;
            state_->push_event(ev);
        },
        .on_compacted = [this](const acp::CompactResultEvent& cr) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::Compacted;
            ev.compact = cr;
            state_->push_event(ev);
        },
        .on_context_stats = [this](const acp::ContextStatsEvent& cs) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::ContextStats;
            ev.context = cs;
            state_->push_event(ev);
        },
        .on_connection = [this](bool /*online*/) {
            // 断线/重连：通知 UI 重绘，状态栏读取 acp_.connected() 反映最新状态
            state_->notify_();
        }
    };
}

void ChatController::connect_sse() {
    acp_.connect(state_->current_session, build_callbacks());
}

// 历史回放：把持久化的消息恢复成 TUI 条目。
// 与实时流一致——user/assistant 纯文本进 history（供构建请求），
// reasoning/tool 仅展示（不入 history，避免把思维链或工具角色发回模型）。
void ChatController::load_history(const std::vector<Message>& msgs) {
    for (auto& m : msgs) {
        if (m.role == "user") {
            state_->add_item(ItemKind::User, m.content);
            state_->history.push_back(m);
        } else if (m.role == "reasoning") {
            state_->add_item(ItemKind::Reasoning, m.content);
        } else if (m.role == "tool") {
            // 合并进匹配的 ToolCall；未匹配则保底为独立条目
            bool matched = false;
            for (auto it = state_->items.rbegin(); it != state_->items.rend(); ++it) {
                if (it->kind == ItemKind::ToolCall && m.tool_call_id && it->tool_id == *m.tool_call_id) {
                    it->has_result = true;
                    it->tool_success = true;
                    it->result_text = m.content;
                    it->folded = tool_auto_fold(*it);
                    matched = true;
                    break;
                }
            }
            if (!matched)
                state_->add_item(ItemKind::ToolResult, m.content);
        } else if (m.role == "assistant") {
            if (m.tool_call_id) {
                json args = m.tool_arguments ? *m.tool_arguments : json::object();
                auto d = tool_display(m.tool_name.value_or(""), args);
                ConvItem item;
                item.kind = ItemKind::ToolCall;
                item.tool_id = *m.tool_call_id;
                item.tool_name = m.tool_name.value_or("");
                item.tool_icon = d.icon;
                item.text = d.label;
                item.tool_pending = d.pending;
                item.tool_title = d.block_title;
                item.tool_block = d.block;
                if (item.tool_name == "write" || item.tool_name == "edit")
                    item.content_text = format_tool_call(item.tool_name, args);
                state_->items.push_back(std::move(item));
                if (state_->notify_) state_->notify_();
            } else {
                state_->add_item(ItemKind::Assistant, m.content);
                state_->history.push_back(m);
            }
        }
    }
}

} // namespace codis