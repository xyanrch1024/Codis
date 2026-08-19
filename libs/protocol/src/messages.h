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
    std::optional<std::string> reasoning_content;  // 思维链（严格 thinking provider 要求回传）

    json to_json() const {
        json j{{"role", role}, {"content", content}};
        if (reasoning_content) j["reasoning_content"] = *reasoning_content;
        if (role == "assistant" && tool_call_id && tool_name) {
            // 工具调用消息 → OpenAI 标准 tool_calls 数组。平铺的 name/arguments
            // 会被部分 provider（GLM 等）忽略，模型看不到自己已调用过该工具，
            // 每轮重复执行同一调用（如反复 write 同一文件）。
            std::string args = tool_arguments ? json_dump_safe(*tool_arguments) : "{}";
            j["content"] = nullptr;
            j["tool_calls"] = json::array({{
                {"id", *tool_call_id},
                {"type", "function"},
                {"function", {{"name", *tool_name}, {"arguments", args}}}
            }});
            // 兼容旧格式的平铺字段（部分宽松 provider 仍读取）
            j["tool_call_id"] = *tool_call_id;
            j["name"] = *tool_name;
            if (tool_arguments) j["arguments"] = *tool_arguments;
        } else if (tool_call_id) {
            // tool 结果消息
            j["tool_call_id"] = *tool_call_id;
        }
        if (tool_name) j["name"] = *tool_name;
        if (tool_arguments) j["arguments"] = *tool_arguments;
        return j;
    }
    static Message from_json(const json& j) {
        Message m;
        m.role = j["role"].get<std::string>();
        // content 可能缺失或为 null（assistant 工具调用消息标准格式），
        // 不能直接 value("content","")——nlohmann 对 null 调 get<string> 会抛
        m.content = (j.contains("content") && j["content"].is_string())
                        ? j["content"].get<std::string>() : std::string();
        if (j.contains("tool_call_id")) m.tool_call_id = j["tool_call_id"].get<std::string>();
        if (j.contains("name")) m.tool_name = j["name"].get<std::string>();
        if (j.contains("arguments")) m.tool_arguments = j["arguments"];
        if (j.contains("reasoning_content") && j["reasoning_content"].is_string())
            m.reasoning_content = j["reasoning_content"].get<std::string>();
        return m;
    }
};

struct ChatRequest {
    std::string provider;            // 空 = 使用 server 配置的 default_provider
    std::string model = "gpt-4o";
    std::string session_id;
    std::vector<Message> messages;
    std::optional<int> max_tokens;
    json tools = json::array();
    bool stream = false;

    json to_json() const {
        json j;
        if (!provider.empty()) j["provider"] = provider;
        j["model"] = model;
        if (!session_id.empty()) j["session_id"] = session_id;
        j["messages"] = json::array();
        for (auto& m : messages) j["messages"].push_back(m.to_json());
        if (max_tokens) j["max_tokens"] = *max_tokens;
        if (!tools.empty()) j["tools"] = tools;
        j["stream"] = stream;
        return j;
    }

    static ChatRequest from_json(const json& j) {
        ChatRequest r;
        if (j.contains("provider")) r.provider = j["provider"].get<std::string>();
        r.model       = j.value("model", "gpt-4o");
        if (j.contains("session_id")) r.session_id = j["session_id"].get<std::string>();
        r.max_tokens  = j.contains("max_tokens")  ? std::optional(j["max_tokens"].get<int>())  : std::nullopt;
        r.stream      = j.value("stream", false);
        if (j.contains("tools")) r.tools = j["tools"];
        if (j.contains("messages")) {
            for (auto& m : j["messages"]) r.messages.push_back(Message::from_json(m));
        }
        return r;
    }
};

// LLM 调用结果错误码（on_done 回调与 ChatResponse 共用）
enum class LlmErrorCode {
    None = 0,     // 成功
    Canceled,     // 客户端主动取消
    RateLimited,  // HTTP 429（限流/配额）
    HttpStatus,   // 其它非 200 状态码
    Network,      // 网络/传输错误
    Parse,        // 响应 JSON 解析失败
};

struct ChatResponse {
    std::string content;
    std::string reasoning_content;  // 思维链（GLM 等模型），解析层透传
    bool success = false;
    LlmErrorCode error_code = LlmErrorCode::None;  // 错误码（错误细分见 enum）
    std::string error;                             // 可读错误消息（展示用）
};

} // namespace codis
