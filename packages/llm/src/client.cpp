#include "client.h"
#include "log.h"

#include <iostream>

namespace opencode {

LLMHttpClient::LLMHttpClient() {}

void LLMHttpClient::stream_post(const std::string& url,
                                const std::string& api_key,
                                const json& body,
                                TokenCallback on_token,
                                DoneCallback on_done,
                                int timeout_seconds,
                                bool non_stream)
{
    httplib::Headers headers = {
        {"Authorization", "Bearer " + api_key},
        {"Content-Type", "application/json"},
        {"User-Agent", "opencode-cpp/0.1.0"}
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

    std::string req_body = body.dump();

    LOG_DEBUG("POST {}://{}{} ({} bytes, stream={})", use_ssl ? "https" : "http", host, path, req_body.size(), !non_stream);

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

            // OpenAI 格式
            if (j.contains("choices") && !j["choices"].empty()) {
                auto& msg = j["choices"][0]["message"];
                if (msg.contains("content") && msg["content"].is_string())
                    content = msg["content"].get<std::string>();
            }
            // Anthropic 格式
            if (j.contains("content") && j["content"].is_array() && !j["content"].empty()) {
                auto& c = j["content"][0];
                if (c.contains("text") && c["text"].is_string())
                    content = c["text"].get<std::string>();
            }

            if (on_done) on_done(content, true, "");
        } catch (const json::parse_error& e) {
            if (on_done) on_done("", false, "JSON parse error: " + std::string(e.what()));
        }
        return;
    }

    // ====================================================================
    // 流式 SSE — POST 请求 (body 中 "stream":true)，解析 SSE 响应行
    // ====================================================================
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

    LOG_TRACE("SSE response {} bytes, parsing...", res->body.size());

    // 逐行解析 SSE
    std::string_view body_view = res->body;
    std::size_t pos;
    while ((pos = body_view.find('\n')) != std::string_view::npos) {
        auto line = body_view.substr(0, pos);
        body_view.remove_prefix(pos + 1);

        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;

        if (line.starts_with("data: ")) {
            auto payload = line.substr(6);
            if (payload == "[DONE]") break;

            try {
                auto j = json::parse(payload);
                if (j.contains("choices") && !j["choices"].empty()) {
                    auto& choice = j["choices"][0];
                    if (choice.contains("delta") && choice["delta"].contains("content")) {
                        auto& c = choice["delta"]["content"];
                        if (c.is_string() && on_token) on_token(c.get<std::string>());
                    }
                }
                if (j.contains("delta") && j["delta"].contains("text")) {
                    if (on_token) on_token(j["delta"]["text"].get<std::string>());
                }
            } catch (const json::parse_error&) {}
        }
    }

    if (on_done) on_done("", true, "");
}

} // namespace opencode
