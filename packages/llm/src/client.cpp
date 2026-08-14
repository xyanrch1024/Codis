#include "client.h"
#include "log.h"

#include <iostream>
#include <map>

namespace codis {

LLMHttpClient::LLMHttpClient() {}

void LLMHttpClient::stream_post(const std::string& url,
                                const std::string& api_key,
                                const json& body,
                                TokenCallback on_token,
                                DoneCallback on_done,
                                int timeout_seconds,
                                bool non_stream,
                                std::string* reasoning_out,
                                ReasoningCallback on_reasoning,
                                std::atomic<bool>* abort_flag)
{
    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key},
        {"Content-Type", "application/json"},
        {"User-Agent", "codis-cpp/0.1.0"}
    };

    std::string url_part = url;
    bool use_ssl = false;

    if (url_part.starts_with("https://")) {
        use_ssl = true;
        url_part = url_part.substr(8);
    } else if (url_part.starts_with("http://")) {
        url_part = url_part.substr(7);
    }

    std::string host, path;
    auto slash_pos = url_part.find('/');
    if (slash_pos != std::string::npos) {
        host = url_part.substr(0, slash_pos);
        path = url_part.substr(slash_pos);
    } else {
        host = url_part;
        path = "/";
    }

    httplib::Client client((use_ssl ? "https://" : "http://") + host);
    client.set_follow_location(true);

    client.set_connection_timeout(timeout_seconds, 0);
    client.set_read_timeout(timeout_seconds, 0);
    client.set_write_timeout(timeout_seconds, 0);

    std::string req_body = json_dump_safe(body);

    // LOG_DEBUG("POST {}://{}{} ({} bytes, stream={}), body={}", use_ssl ? "https" : "http", host, path, req_body.size(), !non_stream, req_body);

    if (non_stream) {
        auto res = client.Post(path, headers, req_body, "application/json");
        if (!res) {
            LOG_ERROR("HTTP POST {} failed: {}", path, httplib::to_string(res.error()));
            if (on_done) on_done("", false, "HTTP error: " + httplib::to_string(res.error()));
            return;
        }
        if (res->status != 200) {
            LOG_WARN("HTTP POST {} returned status {}", path, res->status);
            if (on_done) on_done("", false, "HTTP " + std::to_string(res->status));
            return;
        }
        LOG_TRACE("HTTP response {} bytes", res->body.size());

        try {
            auto j = json::parse(res->body);
            std::string content;
            std::string reasoning;

            // OpenAI 格式
            if (j.contains("choices") && !j["choices"].empty()) {
                auto& msg = j["choices"][0]["message"];
                if (msg.contains("content") && msg["content"].is_string())
                    content = msg["content"].get<std::string>();
                if (msg.contains("reasoning_content") && msg["reasoning_content"].is_string())
                    reasoning = msg["reasoning_content"].get<std::string>();
            }
            // Anthropic 格式
            if (j.contains("content") && j["content"].is_array() && !j["content"].empty()) {
                auto& c = j["content"][0];
                if (c.contains("text") && c["text"].is_string())
                    content = c["text"].get<std::string>();
            }

            if (reasoning_out) *reasoning_out = std::move(reasoning);
            if (on_done) on_done(content, true, "");
        } catch (const json::parse_error& e) {
            if (on_done) on_done("", false, "JSON parse error: " + std::string(e.what()));
        }
        return;
    }

    // ====================================================================
    // 流式 SSE — POST 请求 (body 中 "stream":true)，用 ContentReceiver 逐块
    // 接收，按行实时解析并回调 on_token（不等整个响应收完）
    // ====================================================================
    // 收集 API 原生 tool_calls（按 index 分组）
    std::map<int, json> tool_calls;
    std::string reasoning;  // 思维链增量（GLM 等），不计入 content
    std::string line_buf;   // 跨块残留的半行
    bool stream_done = false;

    auto res = client.Post(
        path, headers, req_body, "application/json",
        [&](const char* data, size_t len) -> bool {
            // 客户端取消：立即停止接收，让 Post 尽快返回（httplib 记为 Canceled）
            if (abort_flag && abort_flag->load()) return false;

            line_buf.append(data, len);

            std::size_t pos;
            while ((pos = line_buf.find('\n')) != std::string::npos) {
                auto line = line_buf.substr(0, pos);
                line_buf.erase(0, pos + 1);

                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                if (!parse_sse_line(line, tool_calls, reasoning, on_token, on_reasoning)) {
                    stream_done = true;  // [DONE]：主动停止接收，其余忽略
                    return false;
                }
            }
            return true;
        });

    // 正常收到 [DONE] 或客户端取消（abort_flag）时，httplib 会把主动停止记为
    // Canceled（"Connection handling canceled"），这是预期的完成信号，不是错误
    if (!res || res->status != 200) {
        bool ok = stream_done || (abort_flag && abort_flag->load());
        if (res && res->status != 200 && !ok) {
            LOG_WARN("HTTP POST {} returned status {}", path, res->status);
            if (on_done) on_done("", false, "HTTP " + std::to_string(res->status));
            return;
        }
        if (ok) {
            LOG_TRACE("HTTP POST {} stream completed ([DONE]{}cancel)", path,
                      stream_done ? "/" : " or ");
            goto done;
        }
        LOG_ERROR("HTTP POST {} failed: {}", path, httplib::to_string(res.error()));
        if (on_done) on_done("", false, "HTTP error: " + httplib::to_string(res.error()));
        return;
    }

done:
    // 拼装 tool_calls JSON 追加到 content
    if (!tool_calls.empty()) {
        json tc_list = json::array();
        for (auto& [idx, entry] : tool_calls) {
            auto args_str = entry.value("arguments", "{}");
            json func = {{"name", entry.value("name", "")}};
            try { func["arguments"] = json::parse(args_str); }
            catch (...) { func["arguments"] = args_str; }
            tc_list.push_back({
                {"id", entry.value("id", "call_" + std::to_string(idx))},
                {"function", func}
            });
        }
        json wrapper = {{"tool_calls", tc_list}};
        if (on_token) on_token(wrapper.dump());
    }

    if (reasoning_out) *reasoning_out = std::move(reasoning);
    // 取消时用保留字 "canceled" 标记，provider 侧据此设 result.canceled。
    // 注意加花括号：else 必须绑定外层 if（abort），否则正常完成时 on_done 不会被调用。
    if (abort_flag && abort_flag->load()) {
        if (on_done) on_done("", true, "__canceled__");
    } else if (on_done) {
        on_done("", true, "");
    }
}

bool LLMHttpClient::parse_sse_line(const std::string& line,
                                   std::map<int, json>& tool_calls,
                                   std::string& reasoning,
                                   TokenCallback& on_token,
                                   ReasoningCallback& on_reasoning) {
    if (!line.starts_with("data: ")) return true;
    auto payload = line.substr(6);
    if (payload == "[DONE]") return false;

    try {
        auto j = json::parse(payload);
        if (j.contains("choices") && !j["choices"].empty()) {
            auto& choice = j["choices"][0];
            if (choice.contains("delta")) {
                auto& delta = choice["delta"];
                if (delta.contains("content")) {
                    auto& c = delta["content"];
                    if (c.is_string() && on_token) on_token(c.get<std::string>());
                }
                if (delta.contains("reasoning_content")) {
                    auto& rc = delta["reasoning_content"];
                    if (rc.is_string()) {
                        auto d = rc.get<std::string>();
                        reasoning += d;
                        if (on_reasoning) on_reasoning(d);
                    }
                }
                if (delta.contains("tool_calls")) {
                    for (auto& tc : delta["tool_calls"]) {
                        int idx = tc.value("index", 0);
                        auto& entry = tool_calls[idx];
                        if (tc.contains("id")) entry["id"] = tc["id"];
                        if (tc.contains("type")) entry["type"] = tc["type"];
                        if (tc.contains("function")) {
                            auto& func = tc["function"];
                            if (func.contains("name")) entry["name"] = func["name"];
                            if (func.contains("arguments")) {
                                std::string prev = entry.value("arguments", "");
                                entry["arguments"] = prev + func["arguments"].get<std::string>();
                            }
                        }
                    }
                }
            }
        }
        if (j.contains("delta") && j["delta"].contains("text")) {
            if (on_token) on_token(j["delta"]["text"].get<std::string>());
        }
    } catch (const json::parse_error&) {}
    return true;
}

} // namespace codis
