#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace codis {

using json = nlohmann::json;

// 把任意字节串转成合法 UTF-8：非法字节/序列替换为 U+FFFD。
// 工具输出、文件读取等可能含二进制字节或跨多字节截断，先清理再入 JSON/DB。
inline std::string make_valid_utf8(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto emit = [&](unsigned c) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    };
    auto invalid = [&] { out += "\xEF\xBF\xBD"; };

    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) { out += static_cast<char>(b); i++; continue; }
        int len;
        unsigned cp;
        if ((b & 0xE0) == 0xC0) { len = 2; cp = b & 0x1F; }
        else if ((b & 0xF0) == 0xE0) { len = 3; cp = b & 0x0F; }
        else if ((b & 0xF8) == 0xF0) { len = 4; cp = b & 0x07; }
        else { invalid(); i++; continue; }

        bool ok = true;
        for (int k = 1; k < len && ok; k++) {
            if (i + static_cast<size_t>(k) >= n) { ok = false; break; }
            unsigned char cb = static_cast<unsigned char>(s[i + k]);
            if ((cb & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cb & 0x3F);
        }
        if (!ok) {
            // 非法序列：一个 U+FFFD，吞掉后续残余的连续字节
            invalid();
            i++;
            while (i < n && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
            continue;
        }
        // 拒绝 overlong / 代理区 / 超界编码
        if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            invalid(); i++; continue;
        }
        emit(cp);
        i += static_cast<size_t>(len);
    }
    return out;
}

// JSON 序列化，非 UTF-8 字节替换为 U+FFFD 而非抛异常（nlohmann 默认 strict 会 throw）
inline std::string json_dump_safe(const json& j, int indent = -1) {
    return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

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
        if (j.contains("arguments")) m.tool_arguments = j["arguments"];
        return m;
    }
};

struct ChatRequest {
    std::string provider;            // 空 = 使用 server 配置的 default_provider
    std::string model = "gpt-4o";
    std::string session_id;
    std::vector<Message> messages;
    std::optional<int> max_tokens;
    std::optional<double> temperature;
    json tools = json::array();
    bool stream = false;

    json to_json() const {
        json j;
        if (!provider.empty()) j["provider"] = provider;
        j["model"] = model;
        j["messages"] = json::array();
        for (auto& m : messages) j["messages"].push_back(m.to_json());
        if (max_tokens) j["max_tokens"] = *max_tokens;
        if (temperature) j["temperature"] = *temperature;
        if (!tools.empty()) j["tools"] = tools;
        j["stream"] = stream;
        return j;
    }

    static ChatRequest from_json(const json& j) {
        ChatRequest r;
        if (j.contains("provider")) r.provider = j["provider"].get<std::string>();
        r.model       = j.value("model", "gpt-4o");
        r.max_tokens  = j.contains("max_tokens")  ? std::optional(j["max_tokens"].get<int>())  : std::nullopt;
        r.temperature = j.contains("temperature") ? std::optional(j["temperature"].get<double>()) : std::nullopt;
        r.stream      = j.value("stream", false);
        if (j.contains("tools")) r.tools = j["tools"];
        if (j.contains("messages")) {
            for (auto& m : j["messages"]) r.messages.push_back(Message::from_json(m));
        }
        return r;
    }
};

struct ChatResponse {
    std::string content;
    std::string reasoning_content;  // 思维链（GLM 等模型），解析层透传
    bool success = false;
    bool canceled = false;          // 被客户端取消（LLM 流被中断）
    std::string error;
};

} // namespace codis
