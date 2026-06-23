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

    json to_json() const { return {{"role", role}, {"content", content}}; }
    static Message from_json(const json& j) {
        return {j["role"].get<std::string>(), j["content"].get<std::string>()};
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
