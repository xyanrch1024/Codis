#include "acp_client.h"

#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

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
    // Step 1: POST /api/v1/acp (fire-and-forget, starts LLM loop)
    auto req_json = request.to_json();
    if (!request.session_id.empty()) req_json["session_id"] = request.session_id;

    httplib::Headers headers = {{"Content-Type", "application/json"}};
    auto post_res = http_->Post("/api/v1/acp", headers, req_json.dump(), "application/json");
    if (!post_res) {
        if (callbacks.on_error) callbacks.on_error("Server unreachable");
        return false;
    }

    std::string sid = request.session_id;
    if (sid.empty()) {
        try {
            auto j = acp::json::parse(post_res->body);
            if (j.is_array() && !j.empty() && j[0].is_array() && j[0].size() >= 2)
                sid = j[0][1].get<std::string>();
        } catch (...) {}
    }
    if (sid.empty()) {
        if (callbacks.on_error) callbacks.on_error("Failed to get session_id");
        return false;
    }

    // Step 2: GET /api/v1/acp/stream/{sid}?skip_history=1 — 阻塞读 SSE 推送
    httplib::Client stream_client(host_, port_);
    stream_client.set_connection_timeout(5, 0);
    stream_client.set_read_timeout(300, 0);

    std::string sse_buf;
    bool got_done = false;

    stream_client.Get(("/api/v1/acp/stream/" + sid + "?skip_history=1").c_str(),
        [&](const char* data, size_t len) {
            sse_buf.append(data, len);
            std::size_t pos;
            while ((pos = sse_buf.find('\n')) != std::string::npos) {
                auto line = sse_buf.substr(0, pos);
                sse_buf.erase(0, pos + 1);
                if (line.empty() || line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (!line.starts_with("data: ")) continue;

                auto event = acp::parse_frame(line);
                if (!event) continue;

                switch (event->type) {
                case acp::EventType::connected:
                    conn_id_ = event->data.value("conn_id", "");
                    break;
                case acp::EventType::assistant:
                    if (callbacks.on_assistant) callbacks.on_assistant(event->data.value("delta", ""));
                    break;
                case acp::EventType::tool_call:
                    if (callbacks.on_tool_call)
                        callbacks.on_tool_call({event->data.value("id",""), event->data.value("name",""),
                            event->data.value("arguments", acp::json::object())});
                    break;
                case acp::EventType::tool_result:
                    if (callbacks.on_tool_result)
                        callbacks.on_tool_result({event->data.value("id",""), event->data.value("success",false),
                            event->data.value("content","")});
                    break;
                case acp::EventType::error:
                    if (callbacks.on_error) callbacks.on_error(event->data.value("message",""));
                    break;
                case acp::EventType::done:
                    if (callbacks.on_done) callbacks.on_done();
                    got_done = true;
                    return false;
                }
            }
            return true;
        });

    if (!got_done && callbacks.on_done) callbacks.on_done();
    return true;
}

bool AcpClient::send_async(const ChatRequest& request) {
    httplib::Headers headers = {{"Content-Type", "application/json"}};
    auto req_json = request.to_json();
    if (!request.session_id.empty()) req_json["session_id"] = request.session_id;
    if (!conn_id_.empty()) req_json["conn_id"] = conn_id_;
    auto res = http_->Post("/api/v1/acp", headers, req_json.dump(), "application/json");
    return res && (res->status == 200 || res->status == 202);
}

// =============================================================================
// 长连接模式 — 后台 SSE 线程, 实时接收广播
// =============================================================================

bool AcpClient::connect(const std::string& session_id, Callbacks callbacks) {
    if (connected_) return false;
    callbacks_ = std::move(callbacks);

    connected_ = true;
    sse_thread_ = std::thread([this, session_id]() {
        int retry_delay = 1;
        int retry_count = 0;

        while (connected_) {
            if (retry_count >= 10) {
                LOG_ERROR("SSE reconnect failed after {} attempts", retry_count);
                if (callbacks_.on_error)
                    callbacks_.on_error("Connection lost, max reconnection attempts reached");
                break;
            }
            httplib::Client client(host_, port_);
            client.set_connection_timeout(5, 0);
            // read_timeout 不设 — keepalive SSE 永不超时

            // 只发一次 GET，服务端 keepalive 模式不主动断开，数据持续推送
            client.Get(("/api/v1/acp/stream/" + session_id + "?keepalive=1").c_str(),
                [&](const char* data, size_t len) {
                    if (!connected_) return false;
                    static thread_local std::string buf;
                    buf.append(data, len);
                    std::size_t pos;
                    while ((pos = buf.find('\n')) != std::string::npos) {
                        auto line = buf.substr(0, pos);
                        buf.erase(0, pos + 1);
                        if (line.empty() || line.back() == '\r') line.pop_back();
                        if (line.empty()) continue;
                        if (line.starts_with("data: ")) {
                            auto event = acp::parse_frame(line);
                            if (!event) continue;
                            switch (event->type) {
                            case acp::EventType::connected:
                                conn_id_ = event->data.value("conn_id", "");
                                LOG_DEBUG("SSE connected, conn_id={}", conn_id_);
                                break;
                            case acp::EventType::assistant:
                                if (callbacks_.on_assistant) callbacks_.on_assistant(event->data.value("delta", ""));
                                break;
                            case acp::EventType::tool_call:
                                if (callbacks_.on_tool_call) callbacks_.on_tool_call({
                                    event->data.value("id",""), event->data.value("name",""),
                                    event->data.value("arguments", acp::json::object())});
                                break;
                            case acp::EventType::tool_result:
                                if (callbacks_.on_tool_result) callbacks_.on_tool_result({
                                    event->data.value("id",""), event->data.value("success",false),
                                    event->data.value("content","")});
                                break;
                            case acp::EventType::error:
                                if (callbacks_.on_error) callbacks_.on_error(event->data.value("message",""));
                                break;
                            case acp::EventType::done:
                                if (callbacks_.on_done) callbacks_.on_done();
                                break;
                            }
                        }
                    }
                    return true;
                });

            if (!connected_) break;

            retry_count++;
            LOG_WARN("SSE disconnected, reconnecting ({}/{}) in {}s...",
                     retry_count, 10, retry_delay);
            if (callbacks_.on_error)
                callbacks_.on_error("Connection lost, reconnecting...");

            std::this_thread::sleep_for(std::chrono::seconds(retry_delay));
            retry_delay = std::min(retry_delay * 2, 30);
        }
    });

    return true;
}

void AcpClient::disconnect() {
    connected_ = false;
    if (sse_thread_.joinable()) sse_thread_.join();
}

std::optional<std::string> AcpClient::create_session() {
    auto res = http_->Post("/api/v1/sessions", "", "");
    if (!res || res->status != 201) return std::nullopt;
    try {
        return acp::json::parse(res->body)["session_id"].get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}

std::vector<SessionInfo> AcpClient::list_sessions() {
    std::vector<SessionInfo> result;
    auto res = http_->Get("/api/v1/sessions");
    if (!res || res->status != 200) return result;
    try {
        auto arr = acp::json::parse(res->body);
        for (auto& j : arr) {
            SessionInfo s;
            s.id = j.value("id", "");
            s.title = j.value("title", "Untitled");
            s.message_count = j.value("message_count", 0);
            s.created_at = j.value("created_at", 0);
            s.updated_at = j.value("updated_at", 0);
            result.push_back(s);
        }
    } catch (...) {}
    return result;
}

std::optional<SessionInfo> AcpClient::get_session(const std::string& id) {
    auto res = http_->Get(("/api/v1/sessions/" + id).c_str());
    if (!res || res->status != 200) return std::nullopt;
    try {
        auto j = acp::json::parse(res->body);
        SessionInfo info;
        info.id = j["id"].get<std::string>();
        if (j.contains("messages")) {
            for (auto& m : j["messages"])
                info.messages.push_back(Message::from_json(m));
        }
        return info;
    } catch (...) { return std::nullopt; }
}

bool AcpClient::delete_session(const std::string& id) {
    auto res = http_->Delete(("/api/v1/sessions/" + id).c_str());
    return res && res->status == 200;
}

bool AcpClient::delete_all_sessions() {
    auto res = http_->Delete("/api/v1/sessions");
    return res && res->status == 200;
}

std::string AcpClient::get_last_session() {
    auto res = http_->Get("/api/v1/sessions");
    if (!res || res->status != 200) return "";
    try {
        auto arr = acp::json::parse(res->body);
        if (!arr.empty()) return arr[0].value("id", "");
    } catch (...) {}
    return "";
}

bool AcpClient::switch_session(const std::string& session_id) {
    json body = {{"conn_id", conn_id_}, {"session_id", session_id}};
    httplib::Headers headers = {{"Content-Type", "application/json"}};
    auto res = http_->Post("/api/v1/acp/switch", headers, body.dump(), "application/json");
    return res && res->status == 200;
}

} // namespace opencode
