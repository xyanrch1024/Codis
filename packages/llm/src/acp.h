#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <optional>
#include <nlohmann/json.hpp>

namespace opencode::acp {

using json = nlohmann::json;

// =============================================================================
// ACP 事件类型
// =============================================================================

enum class EventType {
    connected,    // SSE 连接建立，携带 conn_id
    assistant,    // 模型回复增量文本
    tool_call,    // 模型请求调用工具
    tool_result,  // 工具执行结果
    error,        // 错误
    done          // 对话完成
};

inline std::string to_string(EventType t) {
    switch (t) {
        case EventType::connected:  return "connected";
        case EventType::assistant:  return "assistant";
        case EventType::tool_call:  return "tool_call";
        case EventType::tool_result: return "tool_result";
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

// =============================================================================
// ACP SSE 帧序列化
// =============================================================================

inline std::string to_sse_frame(EventType type, const json& data) {
    json frame;
    frame["type"] = to_string(type);
    frame["data"] = data;
    return "data: " + frame.dump() + "\n\n";
}

inline std::string assistant_frame(std::string_view delta) {
    return to_sse_frame(EventType::assistant, {{"delta", delta}});
}

inline std::string tool_call_frame(const std::string& id,
                                    const std::string& name,
                                    const json& arguments) {
    return to_sse_frame(EventType::tool_call, {
        {"id", id}, {"name", name}, {"arguments", arguments}
    });
}

inline std::string tool_result_frame(const std::string& id,
                                      bool success,
                                      std::string_view content) {
    return to_sse_frame(EventType::tool_result, {
        {"id", id}, {"success", success}, {"content", content}
    });
}

inline std::string error_frame(std::string_view message) {
    return to_sse_frame(EventType::error, {{"message", message}});
}

inline std::string done_frame() {
    return to_sse_frame(EventType::done, json::object());
}

inline std::string connected_frame(const std::string& conn_id) {
    return to_sse_frame(EventType::connected, {{"conn_id", conn_id}});
}

// =============================================================================
// SSE 帧解析
// =============================================================================

struct ParsedEvent {
    EventType type;
    json data;
};

inline std::optional<ParsedEvent> parse_frame(const std::string& sse_line) {
    // 期望格式: data: {"type":"...","data":{...}}
    if (!sse_line.starts_with("data: ")) return std::nullopt;

    auto payload = sse_line.substr(6);
    if (payload == "[DONE]") {
        return ParsedEvent{EventType::done, json::object()};
    }

    try {
        auto j = json::parse(payload);
        auto type_str = j["type"].get<std::string>();

        EventType type;
        if (type_str == "connected")   type = EventType::connected;
        else if (type_str == "assistant")   type = EventType::assistant;
        else if (type_str == "tool_call")   type = EventType::tool_call;
        else if (type_str == "tool_result") type = EventType::tool_result;
        else if (type_str == "error")       type = EventType::error;
        else if (type_str == "done")        type = EventType::done;
        else return std::nullopt;

        return ParsedEvent{type, j["data"]};
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace opencode::acp
