#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace opencode {

using json = nlohmann::json;

// =============================================================================
// 共享类型 — 整个项目通用
// =============================================================================

struct Message {
    std::string role;
    std::string content;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> tool_name;
    std::optional<json> tool_arguments;

    json to_json() const {
        json j{{"role", role}, {"content", content}};
        if (tool_call_id) j["tool_call_id"] = *tool_call_id;
        if (tool_name) j["name"] = *tool_name;
        if (tool_arguments) j["arguments"] = *tool_arguments;
        return j;
    }
    static Message from_json(const json& j) {
        Message m;
        m.role = j["role"].get<std::string>();
        m.content = j.value("content", "");
        if (j.contains("tool_call_id")) m.tool_call_id = j["tool_call_id"].get<std::string>();
        if (j.contains("name")) m.tool_name = j["name"].get<std::string>();
        return m;
    }
};

struct ChatRequest {
    std::string provider = "openai";
    std::string model = "gpt-4o";
    std::vector<Message> messages;
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    bool stream = false;

    json to_json() const {
        json j;
        j["provider"] = provider;
        j["model"] = model;
        j["messages"] = json::array();
        for (auto& m : messages) j["messages"].push_back(m.to_json());
        if (max_tokens) j["max_tokens"] = *max_tokens;
        if (temperature) j["temperature"] = *temperature;
        j["stream"] = stream;
        return j;
    }

    static ChatRequest from_json(const json& j) {
        ChatRequest r;
        r.provider    = j.value("provider", "openai");
        r.model       = j.value("model", "gpt-4o");
        r.max_tokens  = j.contains("max_tokens")  ? std::optional(j["max_tokens"].get<int>())  : std::nullopt;
        r.temperature = j.contains("temperature") ? std::optional(j["temperature"].get<double>()) : std::nullopt;
        r.stream      = j.value("stream", false);
        if (j.contains("messages")) {
            for (auto& m : j["messages"]) r.messages.push_back(Message::from_json(m));
        }
        return r;
    }
};

struct ChatResponse {
    std::string content;
    bool success = false;
    std::string error;
};

} // namespace opencode
