#pragma once

#include "acp_client.h"
#include "messages.h"
#include "tool_format.h"
#include "log.h"

#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>
#include <chrono>

namespace codis {

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

// 工具结果源行数超过该值时结果块自动折叠（仅成功结果；失败恒展开）
inline constexpr int kAutoFoldLines = 10;

struct ConvItem {
    ItemKind kind;
    std::string text;
    bool streaming = false;  // 仅 Assistant：true = 仍在流式累积

    // ---- 工具调用（ItemKind::ToolCall）— openCode 风格展示 ----
    std::string tool_id;        // tool_call id，用于关联 tool_result
    std::string tool_name;      // 工具名
    std::string tool_icon;      // 图标（"$", "→", "←", "✱", "⚙"）
    std::string tool_pending;   // pending 文案
    std::string tool_title;     // 块标题（可为空）
    std::string content_text;   // 块内容（write/edit 的 diff）
    bool tool_block = false;    // 需要块布局
    bool has_result = false;    // tool_result 已到达
    bool tool_success = false;  // 结果成功
    bool folded = false;        // 命令输出块折叠（失败结果恒展开）
    std::string result_text;    // 成功输出（bash 等）
    std::string error_text;     // 失败信息
};

// 工具结果源行数超过阈值时结果块自动折叠（仅成功结果；失败恒展开）。
// 行级判定在结果到达且写入时确定，之后仅由用户单击切换。
inline bool tool_auto_fold(const ConvItem& item) {
    if (!item.tool_success) return false;
    const std::string& src = (item.tool_name == "write" || item.tool_name == "edit")
                                 ? item.content_text
                                 : item.result_text;
    int n = 1;
    for (char c : src)
        if (c == '\n') n++;
    if (item.tool_name == "write" || item.tool_name == "edit") n--;  // 首行 (write <path>) 不计
    return n > kAutoFoldLines;
}

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
    std::chrono::steady_clock::time_point request_start_;
    std::string current_model_;

    // ---- 客户端 pending 队列 ----
    // 任务处理中输入的新消息：仅入队，不进入对话区（避免打乱对话顺序）；
    // 由底部状态栏展示。当前任务完成(Done/Error)后由 on_idle_ 逐条取出发送，
    // 发送时才以正常 User 条目进入对话区（UI 线程访问）。
    std::deque<std::string> pending_queue;
    std::function<void()> on_idle_;

    int pending_count() const { return (int)pending_queue.size(); }

    // 上下文大小（system_prompt + history 全部消息字符数），供状态栏显示。
    // 流式回复尚未进入 history，实时计入其增量，保证每次渲染反映最新上下文。
    std::string context_size_str() const {
        size_t n = system_prompt.size();
        for (auto& m : history) n += m.content.size();
        for (auto& it : items)
            if (it.streaming) n += it.text.size();
        if (n >= 1000)
            return std::to_string(n / 1000) + "." + std::to_string((n % 1000) / 100) + "K";
        return std::to_string(n);
    }

    // 取 pending 预览文本（状态栏展示）：最多 prefix 条 + 总长度限制
    std::string pending_preview(int max_items, int max_chars) const {
        if (pending_queue.empty()) return "";
        std::string out;
        int shown = 0;
        for (auto& t : pending_queue) {
            if (shown >= max_items) break;
            if (shown > 0) out += "  |  ";
            out += t;
            shown++;
        }
        if ((int)pending_queue.size() > shown)
            out += "  (+" + std::to_string(pending_queue.size() - shown) + ")";
        if ((int)out.size() > max_chars) out = out.substr(0, max_chars) + "…";
        return out;
    }

    // 取消任务时调用：丢弃未发送的排队消息（与 server 侧行为一致）
    void clear_pending() {
        pending_queue.clear();
        if (notify_) notify_();
    }

    std::mutex mutex;                // 仅保护 queue_

    // 由 TuiClient::run() 设置，指向 screen.Post（线程安全）
    std::function<void()> notify_;

    // ---- UI 线程直接修改 items（无需锁）----
    void add_item(ItemKind kind, std::string text, bool streaming = false) {
        items.push_back({kind, std::move(text), streaming});
        if (notify_) notify_();
    }

    void add_footer(const std::string& label, double secs) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1fs", secs);
        items.push_back({ItemKind::Status, "↳  " + label + "  " + buf});
        if (notify_) notify_();
    }

    void clear_all() {
        items.clear();
        history.clear();
        processing = false;
        pending_streaming_ = false;
        pending_queue.clear();
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
    bool drain_events() {
        std::deque<AcpEvent> batch;
        {
            std::lock_guard lk(mutex);
            batch.swap(queue_);
        }
        LOG_DEBUG("drain_events batch={}", batch.size());
        for (auto& ev : batch) apply_event(ev);
        return !batch.empty();
    }

    bool queue_empty() {
        std::lock_guard lk(mutex);
        return queue_.empty();
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
        case AcpEvent::Kind::ToolCall: {
            finalize_streaming();
            auto d = tool_display(ev.tool_call.name, ev.tool_call.arguments);
            ConvItem item;
            item.kind = ItemKind::ToolCall;
            item.tool_id = ev.tool_call.id;
            item.tool_name = ev.tool_call.name;
            item.tool_icon = d.icon;
            item.text = d.label;
            item.tool_pending = d.pending;
            item.tool_title = d.block_title;
            item.tool_block = d.block;
            if (ev.tool_call.name == "write" || ev.tool_call.name == "edit")
                item.content_text = format_tool_call(ev.tool_call.name, ev.tool_call.arguments);
            items.push_back(std::move(item));
            break;
        }
        case AcpEvent::Kind::ToolResult: {
            // 按 id 合并进匹配的 ToolCall；未匹配则保底为独立条目
            bool matched = false;
            for (auto it = items.rbegin(); it != items.rend(); ++it) {
                if (it->kind == ItemKind::ToolCall && it->tool_id == ev.tool_result.id) {
                    it->has_result = true;
                    it->tool_success = ev.tool_result.success;
                    if (ev.tool_result.success)
                        it->result_text = ev.tool_result.content;
                    else
                        it->error_text = ev.tool_result.content;
                    if (ev.tool_result.success)
                        it->folded = tool_auto_fold(*it);  // 输出超阈值自动折叠（失败恒展开）
                    matched = true;
                    break;
                }
            }
            if (!matched)
                items.push_back({ItemKind::ToolResult, ev.tool_result.content});
            break;
        }
        case AcpEvent::Kind::Error:
            finalize_streaming();
            items.push_back({ItemKind::Error, ev.text});
            processing = false;
            if (on_idle_) on_idle_();
            break;
        case AcpEvent::Kind::Done: {
            finalize_streaming();
            if (request_start_.time_since_epoch().count() > 0) {
                auto dur = std::chrono::steady_clock::now() - request_start_;
                auto secs = std::chrono::duration<double>(dur).count();
                add_footer(current_model_, secs);
            }
            processing = false;
            if (on_idle_) on_idle_();
            break;
        }
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
    void send_request(const std::string& text);
    void flush_pending();
    void cmd_clear();
    void cmd_delete_all();
    void cmd_balance(const std::string& line);
    void cmd_model(const std::string& line);

    int server_port_;
    std::string model_;
    std::string provider_;
    std::string session_arg_;
    AcpClient acp_;
    std::shared_ptr<TuiState> state_;
    std::function<void()> post_job_;
    std::function<void()> exit_loop_;  // run() 内设置为 screen.ExitLoopClosure()

    // Session / Help overlay
    bool sessions_visible_ = false;
    int session_selected_ = 0;
    std::vector<SessionInfo> session_list_;
    bool help_visible_ = false;
    void switch_session(const SessionInfo& s);
    void connect_sse();
    AcpClient::Callbacks build_callbacks();
    void load_history(const std::vector<Message>& msgs);

    // Conversation scrolling（行级偏移，1 像素 = 1 终端行）
    int scroll_px_ = 0;         // 距顶部偏移行数；0 = 顶部
    int max_scroll_ = 0;        // 每帧由 renderer 更新：最大可滚行数
    bool auto_scroll_ = true;

    // 双击 ESC 取消当前任务（非退出）
    std::chrono::steady_clock::time_point last_escape_;
    int esc_count_ = 0;  // 窗口内累计的 ESC 次数（兼容合并的 "\x1b\x1b"）

    // Spinner 动画帧索引（每渲染推进一次）
    int spinner_frame_ = 0;

    // 命令补全弹窗：输入以 "/" 开头时显示
    bool cmd_palette_visible_ = false;
    int cmd_selected_ = 0;

    // 鼠标拖拽选择（FTXUI 内置选择）→ 松开自动复制
    bool drag_active_ = false;  // 左键按下中
    bool drag_moved_ = false;   // 是否发生了实际拖动（区分点击/拖拽）

    // 状态栏瞬时提示（复制/取消等）；定时线程轮询 notice_pending_ 触发自动消失
    std::string notice_;
    std::chrono::steady_clock::time_point notice_at_;
    std::atomic<bool> notice_pending_{false};
    void show_notice(const std::string& msg);

    // 粘贴检测：bracketed paste 标记（\x1b[200~ ... \x1b[201~）识别粘贴内容，
    // 使粘贴的多行内容按换行插入，而不是被当成多次 Enter 提前发送
    bool in_paste_ = false;
    // 输入事件到达时间：时序兜底，识别快速连续到达的粘贴事件流
    std::chrono::steady_clock::time_point last_event_at_;
};

} // namespace codis
