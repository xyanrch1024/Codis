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

bool AcpClient::send_async(const ChatRequest& request) {
    httplib::Headers headers = {{"Content-Type", "application/json"}};
    auto req_json = request.to_json();
    if (!request.session_id.empty()) req_json["session_id"] = request.session_id;
    if (!conn_id_.empty()) req_json["conn_id"] = conn_id_;
    auto res = http_->Post("/api/v1/acp", headers, req_json.dump(), "application/json");
    LOG_INFO("send_async session={} conn={} status={}",
             request.session_id.substr(0, 8), conn_id_.empty() ? "(none)" : conn_id_.substr(0, 8),
             res ? std::to_string(res->status) : "no-response");
    return res && (res->status == 200 || res->status == 202);
}

// =============================================================================
// 长连接模式 — 后台 WebSocket 线程, 实时接收广播
// =============================================================================

bool AcpClient::connect(const std::string& session_id, Callbacks callbacks) {
    if (connected_) return false;
    callbacks_ = std::move(callbacks);

    connected_ = true;
    thread_done_ = false;
    sse_thread_ = std::thread([this, session_id]() {
        int retry_delay = 1;
        int retry_count = 0;

        while (connected_) {
            if (retry_count >= 10) {
                LOG_ERROR("WS reconnect failed after {} attempts", retry_count);
                if (callbacks_.on_error)
                    callbacks_.on_error("Connection lost, max reconnection attempts reached");
                break;
            }

            auto ws = std::make_unique<httplib::ws::WebSocketClient>(
                "ws://" + host_ + ":" + std::to_string(port_) +
                "/api/v1/acp/ws/" + session_id);
            ws->set_connection_timeout(5, 0);
            // read_timeout 不设 — keepalive WS 永不超时（心跳由 httplib 处理）

            if (!ws->connect()) {
                retry_count++;
                LOG_WARN("WS connect failed ({}/{}), reconnecting in {}s...",
                         retry_count, 10, retry_delay);
                if (callbacks_.on_error)
                    callbacks_.on_error("Connection lost, reconnecting...");
                std::this_thread::sleep_for(std::chrono::seconds(retry_delay));
                retry_delay = std::min(retry_delay * 2, 30);
                continue;
            }

            {
                std::lock_guard lock(ws_mutex_);
                ws_ = std::move(ws);
            }

            LOG_DEBUG("WS connected, session={}", session_id.substr(0, 8));

            std::string msg;
            while (connected_) {
                auto r = ws_->read(msg);
                if (r == httplib::ws::ReadResult::Fail) break;
                LOG_DEBUG("WS recv {} bytes: {}",
                          msg.size(),
                          msg.size() <= 160 ? msg : msg.substr(0, 160) + "...");
                auto event = acp::parse_frame(msg);
                if (!event) {
                    LOG_WARN("WS frame parse failed: {}", msg);
                    continue;
                }
                LOG_DEBUG("WS event type={}", acp::to_string(event->type));
                switch (event->type) {
                case acp::EventType::connected:
                    conn_id_ = event->data.value("conn_id", "");
                    LOG_INFO("WS connected, conn_id={}", conn_id_);
                    break;
                case acp::EventType::assistant:
                    LOG_DEBUG("WS assistant delta ({} bytes)", event->data.value("delta", "").size());
                    if (callbacks_.on_assistant) callbacks_.on_assistant(event->data.value("delta", ""));
                    break;
                case acp::EventType::reasoning:
                    LOG_DEBUG("WS reasoning delta ({} bytes)", event->data.value("delta", "").size());
                    if (callbacks_.on_reasoning) callbacks_.on_reasoning(event->data.value("delta", ""));
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
                    LOG_DEBUG("WS done");
                    if (callbacks_.on_done) callbacks_.on_done();
                    break;
                }
            }

            {
                std::lock_guard lock(ws_mutex_);
                ws_.reset();
            }

            if (!connected_) break;

            retry_count++;
            LOG_WARN("WS disconnected, reconnecting ({}/{}) in {}s...",
                     retry_count, 10, retry_delay);
            if (callbacks_.on_error)
                callbacks_.on_error("Connection lost, reconnecting...");

            std::this_thread::sleep_for(std::chrono::seconds(retry_delay));
            retry_delay = std::min(retry_delay * 2, 30);
        }

        thread_done_ = true;
    });

    return true;
}

void AcpClient::disconnect() {
    connected_ = false;
    {
        // 关闭 WS 使阻塞的 read() 返回，唤醒后台线程
        std::lock_guard lock(ws_mutex_);
        if (ws_) ws_->close();
    }
    if (sse_thread_.joinable()) {
        // 有界等待：httplib close() 跨线程有读竞争，超时则 detach，避免挂死
        for (int i = 0; i < 80 && !thread_done_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (thread_done_.load())
            sse_thread_.join();
        else
            sse_thread_.detach();
    }
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
