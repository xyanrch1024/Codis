#pragma once

#include "messages.h"

#include <string>
#include <string_view>
#include <optional>
#include <nlohmann/json.hpp>

namespace codis::acp {

using json = nlohmann::json;

// =============================================================================
// ACP 事件类型
// =============================================================================

enum class EventType {
    connected,    // WS 连接建立，携带 conn_id
    request,      // 客户端 → 服务端：发送一次对话请求（全双工）
    switch_session, // 客户端 → 服务端：切换 conn 到其它 session
    cancel,       // 客户端 → 服务端：取消当前正在执行的任务（LLM 流/工具循环）
    compact,      // 客户端 → 服务端：请求压缩会话历史（保留尾部窗口，头部压成摘要）
    compacted,    // 服务端 → 客户端：压缩完成回执（ok / before / after / 摘要）
    context_stats,// 服务端 → 客户端：向 LLM 发出请求时推送 context 统计（used / max）
    assistant,    // 模型回复增量文本
    reasoning,    // 模型思维链增量文本（reasoning_content）
    tool_call,    // 模型请求调用工具
    tool_result,  // 工具执行结果
    tool_confirm, // 服务端 → 客户端：Ask 权限工具执行前征询确认
    confirm_ack,  // 客户端 → 服务端：确认回执（approved / rejected）
    error,        // 错误
    done          // 对话完成
};

inline std::string to_string(EventType t) {
    switch (t) {
        case EventType::connected:  return "connected";
        case EventType::request:    return "request";
        case EventType::switch_session: return "switch";
        case EventType::cancel:     return "cancel";
        case EventType::compact:    return "compact";
        case EventType::compacted:  return "compacted";
        case EventType::context_stats: return "context_stats";
        case EventType::assistant:  return "assistant";
        case EventType::reasoning:  return "reasoning";
        case EventType::tool_call:  return "tool_call";
        case EventType::tool_result: return "tool_result";
        case EventType::tool_confirm: return "tool_confirm";
        case EventType::confirm_ack:  return "confirm_ack";
        case EventType::error:      return "error";
        case EventType::done:       return "done";
    }
    return "unknown";
}

// =============================================================================
// ACP 事件数据结构
// =============================================================================

struct AssistantEvent {
    std::string delta;        // 增量文本
};

struct ToolCallEvent {
    std::string id;           // tool call id
    std::string name;         // 工具名
    json arguments;           // 参数
};

struct ToolResultEvent {
    std::string id;           // 对应 tool call id
    bool success = false;
    std::string content;      // 结果文本
};

struct ErrorEvent {
    std::string message;
    std::optional<std::string> code;
};

// 上下文压缩回执（服务端 → 客户端）：只携带 LLM 生成的摘要内容
struct CompactResultEvent {
    bool ok = false;
    std::string summary;        // LLM 生成的摘要（失败时为空）
    std::string error;          // 失败原因
};

// 上下文统计（服务端 → 客户端）：每次向 LLM 发出请求时推送
struct ContextStatsEvent {
    int64_t used = 0;           // 本次请求估算 tokens（len/4 近似）
    int64_t max = 0;            // 模型上下文窗口上限（来自 provider 配置）
};

// =============================================================================
// ACP WebSocket 帧序列化（WS 消息天然分帧，直接发 JSON，无 SSE 信封）
// =============================================================================

inline std::string to_frame(EventType type, const json& data) {
    json frame;
    frame["type"] = to_string(type);
    frame["data"] = data;
    return json_dump_safe(frame);
}

inline std::string assistant_frame(std::string_view delta) {
    return to_frame(EventType::assistant, {{"delta", delta}});
}

inline std::string reasoning_frame(std::string_view delta) {
    return to_frame(EventType::reasoning, {{"delta", delta}});
}

inline std::string tool_call_frame(const std::string& id,
                                    const std::string& name,
                                    const json& arguments) {
    return to_frame(EventType::tool_call, {
        {"id", id}, {"name", name}, {"arguments", arguments}
    });
}

inline std::string tool_result_frame(const std::string& id,
                                      bool success,
                                      std::string_view content) {
    return to_frame(EventType::tool_result, {
        {"id", id}, {"success", success}, {"content", content}
    });
}

// 服务端 → 客户端：Ask 权限工具执行前征询确认
// timeout_seconds：客户端显示倒计时用（服务端以此超时视为拒绝）
inline std::string tool_confirm_frame(const std::string& confirm_id,
                                      const std::string& id,
                                      const std::string& name,
                                      const json& arguments,
                                      int timeout_seconds = 120) {
    return to_frame(EventType::tool_confirm, {
        {"confirm_id", confirm_id},
        {"timeout_seconds", timeout_seconds},
        {"call", {{"id", id}, {"name", name}, {"arguments", arguments}}}
    });
}

// 客户端 → 服务端：确认回执
inline std::string confirm_ack_frame(const std::string& confirm_id, bool approved) {
    return to_frame(EventType::confirm_ack, {
        {"confirm_id", confirm_id}, {"approved", approved}
    });
}

inline std::string error_frame(std::string_view message) {
    return to_frame(EventType::error, {{"message", message}});
}

inline std::string done_frame() {
    return to_frame(EventType::done, json::object());
}

inline std::string connected_frame(const std::string& conn_id) {
    return to_frame(EventType::connected, {{"conn_id", conn_id}});
}

// 客户端 → 服务端：全双工对话请求帧
inline std::string request_frame(const ChatRequest& req) {
    json data = req.to_json();
    if (!req.session_id.empty()) data["session_id"] = req.session_id;
    return to_frame(EventType::request, data);
}

// 客户端 → 服务端：切换 conn 到其它 session（conn_id 由服务端从 WS 连接获取）
inline std::string switch_frame(const std::string& session_id) {
    return to_frame(EventType::switch_session, {{"session_id", session_id}});
}

// 客户端 → 服务端：取消当前任务（LLM 流 + 工具循环）
inline std::string cancel_frame(const std::string& session_id) {
    return to_frame(EventType::cancel, {{"session_id", session_id}});
}

// 客户端 → 服务端：请求上下文压缩。keep：保留尾部原文条数（默认 20）
inline std::string compact_frame(const std::string& session_id, int keep = 20) {
    return to_frame(EventType::compact, {{"session_id", session_id}, {"keep", keep}});
}

// 服务端 → 客户端：上下文压缩回执（仅 LLM 摘要内容）
inline std::string compacted_frame(bool ok,
                                   const std::string& summary, const std::string& error) {
    return to_frame(EventType::compacted, {
        {"ok", ok}, {"summary", summary}, {"error", error}
    });
}

// 服务端 → 客户端：context 统计（仅用于状态栏展示，客户端不做任何计算）
inline std::string context_stats_frame(int64_t used, int64_t max) {
    return to_frame(EventType::context_stats, {
        {"used", used}, {"max", max}
    });
}

// =============================================================================
// ACP 帧解析（输入为裸 JSON，如 {"type":"assistant","data":{"delta":"..."}}）
// =============================================================================

struct ParsedEvent {
    EventType type;
    json data;
};

inline std::optional<ParsedEvent> parse_frame(const std::string& payload) {
    try {
        auto j = json::parse(payload);
        auto type_str = j["type"].get<std::string>();

        EventType type;
        if (type_str == "connected")   type = EventType::connected;
        else if (type_str == "request") type = EventType::request;
        else if (type_str == "switch")  type = EventType::switch_session;
        else if (type_str == "cancel")  type = EventType::cancel;
        else if (type_str == "compact") type = EventType::compact;
        else if (type_str == "compacted") type = EventType::compacted;
        else if (type_str == "context_stats") type = EventType::context_stats;
        else if (type_str == "assistant")   type = EventType::assistant;
        else if (type_str == "reasoning")   type = EventType::reasoning;
        else if (type_str == "tool_call")   type = EventType::tool_call;
        else if (type_str == "tool_result") type = EventType::tool_result;
        else if (type_str == "tool_confirm") type = EventType::tool_confirm;
        else if (type_str == "confirm_ack")  type = EventType::confirm_ack;
        else if (type_str == "error")       type = EventType::error;
        else if (type_str == "done")        type = EventType::done;
        else return std::nullopt;

        return ParsedEvent{type, j["data"]};
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace codis::acp
