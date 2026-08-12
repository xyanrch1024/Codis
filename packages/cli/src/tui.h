#pragma once

#include "acp_client.h"
#include "types.h"
#include "tool_format.h"
#include "log.h"

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <functional>
#include <memory>

namespace opencode {

// =============================================================================
// 消息模型 — 渲染的唯一事实源。无控制字符、无前缀嗅探。
// =============================================================================

enum class ItemKind {
    User,       // 用户消息
    Assistant,  // 模型回复
    Reasoning,  // 模型思维链
    ToolCall,   // 工具调用
    ToolResult, // 工具执行结果
    Error,      // 错误
    Status,     // 本地状态消息（切换 session、balance 等）
};

struct ConvItem {
    ItemKind kind;
    std::string text;
    bool streaming = false;  // 仅 Assistant：true = 仍在流式累积
};

// =============================================================================
// ACP 事件 — WS 回调线程只 push 事件，UI 线程 drain 后改 items
// =============================================================================

struct AcpEvent {
    enum class Kind { AssistantDelta, ReasoningDelta, ToolCall, ToolResult, Error, Done };
    Kind kind;
    std::string text;                         // delta / error message
    acp::ToolCallEvent tool_call;             // Kind == ToolCall
    acp::ToolResultEvent tool_result;         // Kind == ToolResult
};

inline const char* acp_event_kind_str(AcpEvent::Kind k) {
    switch (k) {
        case AcpEvent::Kind::AssistantDelta: return "AssistantDelta";
        case AcpEvent::Kind::ReasoningDelta: return "ReasoningDelta";
        case AcpEvent::Kind::ToolCall:       return "ToolCall";
        case AcpEvent::Kind::ToolResult:     return "ToolResult";
        case AcpEvent::Kind::Error:          return "Error";
        case AcpEvent::Kind::Done:           return "Done";
    }
    return "?";
}

struct TuiState {
    std::vector<ConvItem> items;     // 仅 UI 线程修改
    std::vector<Message> history;    // 用于构建请求
    std::string current_session;
    std::string model;
    int server_port = 8711;
    std::string system_prompt = "You are a helpful AI coding assistant.";
    bool processing = false;
    std::string status_msg;

    std::mutex mutex;                // 仅保护 queue_

    // 由 TuiClient::run() 设置，指向 screen.Post（线程安全）
    std::function<void()> notify_;

    // ---- UI 线程直接修改 items（无需锁）----
    void add_item(ItemKind kind, std::string text, bool streaming = false) {
        items.push_back({kind, std::move(text), streaming});
        if (notify_) notify_();
    }

    void clear_all() {
        items.clear();
        history.clear();
        processing = false;
        pending_streaming_ = false;
        if (notify_) notify_();
    }

    // ---- WS 线程入口：入队 + 唤醒 UI ----
    void push_event(const AcpEvent& ev) {
        {
            std::lock_guard lk(mutex);
            queue_.push_back(ev);
            LOG_DEBUG("push_event queued kind={} queue_size={}",
                      acp_event_kind_str(ev.kind), queue_.size());
        }
        if (notify_) notify_();
    }

    // ---- UI 线程每帧调用：吞掉队列并逐个应用到 items ----
    void drain_events() {
        std::deque<AcpEvent> batch;
        {
            std::lock_guard lk(mutex);
            batch.swap(queue_);
        }
        LOG_DEBUG("drain_events batch={}", batch.size());
        for (auto& ev : batch) apply_event(ev);
    }

private:
    std::deque<AcpEvent> queue_;
    bool pending_streaming_ = false;

    void apply_event(const AcpEvent& ev) {
        LOG_DEBUG("apply_event kind={} items={}", acp_event_kind_str(ev.kind), items.size());
        switch (ev.kind) {
        case AcpEvent::Kind::AssistantDelta:
            if (pending_streaming_ && !items.empty()) {
                items.back().text += ev.text;
            } else {
                items.push_back({ItemKind::Assistant, ev.text, true});
                pending_streaming_ = true;
            }
            break;
        case AcpEvent::Kind::ReasoningDelta:
            if (!items.empty() && items.back().kind == ItemKind::Reasoning) {
                items.back().text += ev.text;
            } else {
                items.push_back({ItemKind::Reasoning, ev.text});
            }
            break;
        case AcpEvent::Kind::ToolCall:
            finalize_streaming();
            items.push_back({ItemKind::ToolCall,
                             format_tool_call(ev.tool_call.name, ev.tool_call.arguments)});
            break;
        case AcpEvent::Kind::ToolResult:
            items.push_back({ItemKind::ToolResult, ev.tool_result.content});
            break;
        case AcpEvent::Kind::Error:
            finalize_streaming();
            items.push_back({ItemKind::Error, ev.text});
            processing = false;
            break;
        case AcpEvent::Kind::Done:
            finalize_streaming();
            processing = false;
            break;
        }
        if (notify_) notify_();
    }

    void finalize_streaming() {
        if (!pending_streaming_) return;
        if (!items.empty()) {
            items.back().streaming = false;
            history.push_back({"assistant", items.back().text});
        }
        pending_streaming_ = false;
    }
};

class TuiClient {
public:
    TuiClient(int server_port, std::string model, std::string provider,
              std::string session_arg);
    int run();

private:
    void send_message(const std::string& text);
    void cmd_clear();
    void cmd_delete_all();
    void cmd_balance(const std::string& line);

    int server_port_;
    std::string model_;
    std::string provider_;
    std::string session_arg_;
    AcpClient acp_;
    std::shared_ptr<TuiState> state_;
    std::function<void()> post_job_;
    std::function<void()> exit_loop_;  // run() 内设置为 screen.ExitLoopClosure()

    // Session overlay
    bool sessions_visible_ = false;
    int session_selected_ = 0;
    std::vector<SessionInfo> session_list_;
    void switch_session(const SessionInfo& s);
    void connect_sse();
    AcpClient::Callbacks build_callbacks();

    // Conversation scrolling（按 item 索引）
    int scroll_item_ = -1;  // -1 = auto-scroll to bottom
    bool auto_scroll_ = true;
};

} // namespace opencode
