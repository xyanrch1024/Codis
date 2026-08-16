#include "acp_client.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>

namespace codis {

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
    auto frame = acp::request_frame(request);

    {
        std::lock_guard lock(ws_mutex_);
        if (ws_ && ws_->is_open()) {
            bool ok = ws_->send(frame);
            LOG_DEBUG("send_async over WS session={} conn={} ok={}",
                      request.session_id.substr(0, 8), conn_id_.empty() ? "(none)" : conn_id_.substr(0, 8), ok);
            return ok;
        }
    }

    // WS 未就绪（未连接/重连中）：入队，connect 成功后 flush，不丢消息
    LOG_INFO("send_async queued (WS not ready) session={} conn={}",
             request.session_id.substr(0, 8), conn_id_.empty() ? "(none)" : conn_id_.substr(0, 8));
    {
        std::lock_guard lock(pending_mutex_);
        pending_outbound_.push_back(std::move(frame));
    }
    return true;
}

void AcpClient::send_confirmation(const std::string& confirm_id, bool approved) {
    std::string frame = acp::confirm_ack_frame(confirm_id, approved);
    {
        std::lock_guard lock(ws_mutex_);
        if (ws_ && ws_->is_open()) {
            ws_->send(frame);
            LOG_DEBUG("confirm_ack sent confirm_id={} approved={}", confirm_id, approved);
            return;
        }
    }
    // WS 未就绪：入队等重连后补发
    LOG_INFO("confirm_ack queued (WS not ready) confirm_id={} approved={}", confirm_id, approved);
    {
        std::lock_guard lock(pending_mutex_);
        pending_outbound_.push_back(std::move(frame));
    }
}

// =============================================================================
// 长连接模式 — 后台 WebSocket 线程, 实时接收广播
// =============================================================================

void AcpClient::flush_pending() {
    std::deque<std::string> queued;
    {
        std::lock_guard lock(pending_mutex_);
        queued.swap(pending_outbound_);
    }
    if (queued.empty()) return;

    std::lock_guard lock(ws_mutex_);
    if (!ws_ || !ws_->is_open()) {
        // 仍未就绪：放回队首，等下一次 flush
        std::lock_guard plock(pending_mutex_);
        for (auto it = queued.rbegin(); it != queued.rend(); ++it)
            pending_outbound_.push_front(*it);
        return;
    }
    for (auto& frame : queued) {
        LOG_INFO("flush pending request ({} bytes)", frame.size());
        if (!ws_->send(frame)) {
            LOG_WARN("flush pending request failed, requeueing");
            std::lock_guard plock(pending_mutex_);
            for (auto it = std::find(queued.begin(), queued.end(), frame); it != queued.end(); ++it)
                pending_outbound_.push_front(*it);
            return;
        }
    }
}

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
                    flush_pending();
                    break;
                case acp::EventType::request:
                case acp::EventType::switch_session:
                    // 客户端不会收到这两类帧（上行专用）
                    LOG_WARN("WS received uplink-only frame: {}", acp::to_string(event->type));
                    break;
                case acp::EventType::assistant:
                    LOG_DEBUG("WS assistant delta ({} bytes)", event->data.value("delta", "").size());
                    if (callbacks_.on_assistant) callbacks_.on_assistant(event->data.value("delta", ""));
                    break;
                case acp::EventType::reasoning:
                    // 服务端已过滤空 delta；若仍收到空帧，说明是旧版服务端或第三方客户端
                    if (event->data.value("delta", "").empty()) {
                        LOG_WARN("WS reasoning frame with empty delta");
                        break;
                    }
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
                case acp::EventType::tool_confirm: {
                    if (callbacks_.on_tool_confirm) {
                        auto& call_json = event->data["call"];
                        callbacks_.on_tool_confirm(
                            event->data.value("confirm_id", ""),
                            {call_json.value("id", ""), call_json.value("name", ""),
                             call_json.value("arguments", acp::json::object())},
                            event->data.value("timeout_seconds", 120));
                    }
                    break;
                }
                case acp::EventType::confirm_ack:
                    // 上行专用帧，客户端不会收到
                    LOG_WARN("WS received uplink-only frame: confirm_ack");
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
    } catch (const std::exception& e) {
        LOG_WARN("get_session {}: parse failed: {}", id.substr(0, 8), e.what());
        return std::nullopt;
    }
}

bool AcpClient::delete_session(const std::string& id) {
    auto res = http_->Delete(("/api/v1/sessions/" + id).c_str());
    return res && res->status == 200;
}

bool AcpClient::delete_all_sessions() {
    auto res = http_->Delete("/api/v1/sessions");
    return res && res->status == 200;
}

void AcpClient::cancel_session(const std::string& session_id) {
    auto frame = acp::cancel_frame(session_id);
    {
        std::lock_guard lock(ws_mutex_);
        if (ws_ && ws_->is_open()) {
            ws_->send(frame);
            return;
        }
    }
    LOG_WARN("cancel_session queued (WS not ready) session={}", session_id.substr(0, 8));
    {
        std::lock_guard lock(pending_mutex_);
        pending_outbound_.push_back(std::move(frame));
    }
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

std::optional<ServerInfo> AcpClient::get_server_info() {
    auto res = http_->Get("/api/v1/info");
    if (!res || res->status != 200) return std::nullopt;
    try {
        auto j = acp::json::parse(res->body);
        ServerInfo info;
        info.default_provider = j.value("default_provider", "");
        if (j.contains("providers")) {
            for (auto& p : j["providers"]) info.providers.push_back(p.get<std::string>());
        }
        if (j.contains("provider_models")) {
            for (auto& [k, v] : j["provider_models"].items())
                info.provider_models[k] = v.get<std::string>();
        }
        return info;
    } catch (...) {}
    return std::nullopt;
}

bool AcpClient::switch_session(const std::string& session_id) {
    auto frame = acp::switch_frame(session_id);

    {
        std::lock_guard lock(ws_mutex_);
        if (ws_ && ws_->is_open()) {
            bool ok = ws_->send(frame);
            LOG_DEBUG("switch_session over WS target={} ok={}", session_id.substr(0, 8), ok);
            return ok;
        }
    }

    // WS 未就绪：入队，connect 成功后按序补发
    LOG_INFO("switch_session queued (WS not ready) target={}", session_id.substr(0, 8));
    {
        std::lock_guard lock(pending_mutex_);
        pending_outbound_.push_back(std::move(frame));
    }
    return true;
}

} // namespace codis
