#include "acp_client.h"

#include <iostream>
#include <sstream>

namespace opencode {

AcpClient::AcpClient(int server_port)
    : host_("127.0.0.1")
    , port_(server_port)
    , http_(std::make_unique<httplib::Client>(host_, port_))
{
    http_->set_connection_timeout(5, 0);
    http_->set_read_timeout(300, 0);
    http_->set_write_timeout(10, 0);
}

bool AcpClient::health_check() {
    auto res = http_->Get("/api/v1/health");
    return res && res->status == 200;
}

// =============================================================================
// ACP 对话 — 核心方法
// =============================================================================

bool AcpClient::send(const ChatRequest& request, Callbacks callbacks) {
    httplib::Headers headers = {
        {"Content-Type", "application/json"},
        {"Accept", "text/event-stream"}
    };

    std::string sse_buffer;

    // cpp-httplib 客户端流式读取：通过 body 解析 SSE
    // 使用 httplib::Client::send 获取原始响应体

    httplib::Request hreq;
    hreq.method = "POST";
    hreq.path = "/api/v1/acp";
    hreq.set_header("Content-Type", "application/json");
    hreq.set_header("Accept", "text/event-stream");
    hreq.body = request.to_json().dump();

    httplib::Response hres;
    httplib::Error err;

    LOG_DEBUG("ACP POST /api/v1/acp provider={} model={} msgs={}",
              request.provider, request.model, request.messages.size());

    bool ok = http_->send(hreq, hres, err);

    if (!ok) {
        LOG_ERROR("ACP send failed: {}", httplib::to_string(err));
        if (callbacks.on_error)
            callbacks.on_error("Server unreachable: " + httplib::to_string(err));
        return false;
    }

    if (hres.status != 200) {
        if (callbacks.on_error)
            callbacks.on_error("HTTP " + std::to_string(hres.status) + ": " + hres.body);
        return false;
    }

    // 解析 SSE body
    std::istringstream stream(hres.body);
    std::string line;
    bool got_done = false;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (line.starts_with("data: ")) {
            auto payload = line.substr(6);
            if (payload == "[DONE]") {
                if (callbacks.on_done) callbacks.on_done();
                got_done = true;
                break;
            }

            auto event = acp::parse_frame(line);
            if (!event) continue;

            switch (event->type) {
            case acp::EventType::assistant:
                if (callbacks.on_assistant)
                    callbacks.on_assistant(event->data.value("delta", ""));
                break;
            case acp::EventType::tool_call:
                if (callbacks.on_tool_call) {
                    acp::ToolCallEvent tc{
                        event->data.value("id", ""),
                        event->data.value("name", ""),
                        event->data.value("arguments", acp::json::object())
                    };
                    callbacks.on_tool_call(tc);
                }
                break;
            case acp::EventType::tool_result:
                if (callbacks.on_tool_result) {
                    acp::ToolResultEvent tr{
                        event->data.value("id", ""),
                        event->data.value("success", false),
                        event->data.value("content", "")
                    };
                    callbacks.on_tool_result(tr);
                }
                break;
            case acp::EventType::error:
                if (callbacks.on_error)
                    callbacks.on_error(event->data.value("message", "unknown error"));
                break;
            case acp::EventType::done:
                if (callbacks.on_done) callbacks.on_done();
                got_done = true;
                break;
            }
        }
    }

    if (!got_done && callbacks.on_done) {
        callbacks.on_done();
    }

    return true;
}

// =============================================================================
// 会话管理（复用 REST）
// =============================================================================

std::optional<std::string> AcpClient::create_session() {
    auto res = http_->Post("/api/v1/sessions", "", "");
    if (!res || res->status != 201) return std::nullopt;
    try {
        return acp::json::parse(res->body)["session_id"].get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SessionInfo> AcpClient::get_session(const std::string& id) {
    auto res = http_->Get(("/api/v1/sessions/" + id).c_str());
    if (!res || res->status != 200) return std::nullopt;
    try {
        auto j = acp::json::parse(res->body);
        SessionInfo info;
        info.id       = j["id"].get<std::string>();
        info.model    = j.value("model", "");
        info.provider = j.value("provider", "");
        if (j.contains("messages")) {
            for (auto& m : j["messages"])
                info.messages.push_back({
                    m["role"].get<std::string>(),
                    m["content"].get<std::string>()
                });
        }
        return info;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace opencode
