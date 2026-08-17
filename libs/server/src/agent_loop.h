// AgentLoop — ACP 任务循环与上下文压缩。
// 职责：LLM 多轮对话（turn 循环/重试/权限/工具执行/确认等待）、任务级异常防护、
// 排队请求迭代补跑、头部历史 LLM 摘要压缩。所有副作用经依赖注入的组件访问，
// 可脱离 HTTP/WS 单独单测（见 tests/acp_task_test.cpp）。

#pragma once

#include "config.h"
#include "context_source.h"
#include "provider_registry.h"
#include "session_hub.h"
#include "session_store.h"
#include "tool_registry.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace codis {

// provider 未配置 max_context 时的回退上下文窗口（tokens）
inline constexpr int64_t kDefaultMaxContext = 128000;

// 给定 provider 的配置上下文窗口（未配置 → kDefaultMaxContext）
inline int64_t provider_max_context(const AppConfig& config, ProviderRegistry& providers,
                                    const std::string& provider_name) {
    auto cfg = config.provider_for(provider_name);
    if (cfg && cfg->max_context_tokens) return *cfg->max_context_tokens;
    return kDefaultMaxContext;
}

class AgentLoop {
public:
    AgentLoop(SessionHub& hub, SessionStore& store, SystemContext& system,
              ToolRegistry& tools, ProviderRegistry& providers, const AppConfig& config);

    // 顶层任务入口：单次任务（含异常防护）执行完毕后迭代补跑排队请求。
    // 任务开始时通过 hub 抢占 Running 相位（调用方须先用 hub.start_task 判空）。
    void run_task(const std::string& session_id, const std::string& conn_id, ChatRequest req);
    // 上下文压缩：头部历史 → LLM 摘要 + 保留尾部窗口，写回 session_store。
    // 完成后恢复 Idle；若有排队请求返回第一个（调用方另行补跑，本方法不启动线程）
    std::optional<ChatRequest> run_compact(const std::string& session_id, int keep);

    // 非流式 LLM 调用（/api/v1/chat 与压缩摘要共用）；失败抛 std::runtime_error
    std::string call_llm(const ChatRequest& req);

    // 测试钩子：turn 上限（默认 100）
    void set_max_turns(int n) { max_turns_ = n; }

private:
    // 单轮任务：turn 循环、广播、LLM 流、重试、工具执行（原 run_acp_task）
    void run_task_inner(const std::string& session_id, const std::string& conn_id,
                        ChatRequest req);

    // Ask 权限工具：广播 tool_confirm 帧并挂起，等待 confirm_ack 回执。
    // 返回 true=批准执行；false=拒绝/超时/任务被取消。
    bool wait_for_confirmation(const std::string& session_id, const ToolCall& call,
                               const std::function<void(const std::string&)>& broadcast,
                               const std::shared_ptr<std::atomic<bool>>& cancel_flag);

    std::shared_ptr<LLMProvider> resolve_provider(const ChatRequest& req);

    SessionHub& hub_;
    SessionStore& store_;
    SystemContext& system_;
    ToolRegistry& tools_;
    ProviderRegistry& providers_;
    const AppConfig& config_;
    int max_turns_ = 100;
    static constexpr int kMaxEmptyRetries = 2;
    static constexpr int kMaxMalformedRetries = 3;
};

} // namespace codis
