#include "feishu_ws_client.h"
#include "log.h"
#include <httplib.h>
#include <chrono>

namespace opencode {

FeishuWSClient::FeishuWSClient(const std::string& app_id, const std::string& app_secret)
    : app_id_(app_id), app_secret_(app_secret)
{}

FeishuWSClient::~FeishuWSClient() { stop(); }

// =============================================================================
// Tenant Access Token
// =============================================================================

std::string FeishuWSClient::get_tenant_token() {
    httplib::SSLClient cli("open.feishu.cn");
    json body{{"app_id", app_id_}, {"app_secret", app_secret_}};

    auto res = cli.Post("/open-apis/auth/v3/tenant_access_token/internal",
        body.dump(), "application/json");

    if (!res || res->status != 200) {
        LOG_ERROR("Failed to get tenant token: {}",
                   res ? std::to_string(res->status) : "no response");
        return "";
    }

    auto j = json::parse(res->body);
    if (j.value("code", -1) != 0) {
        LOG_ERROR("Token error: {}", j.value("msg", "unknown"));
        return "";
    }

    token_ = j["tenant_access_token"].get<std::string>();
    LOG_INFO("Tenant token obtained, expires in {}s", j.value("expire", 0));
    return token_;
}

// =============================================================================
// WebSocket 长连接
// =============================================================================

void FeishuWSClient::start() {
    running_ = true;
    worker_ = std::thread([this] { run_loop(); });
}

void FeishuWSClient::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

bool FeishuWSClient::connect_ws() {
    httplib::Headers headers = {{"Authorization", "Bearer " + token_}};
    httplib::ws::WebSocketClient ws("wss://open.feishu.cn/open-apis/ws/path?v=v1", headers);

    if (!ws.connect()) {
        LOG_ERROR("WebSocket connect failed");
        return false;
    }

    connected_ = true;
    LOG_INFO("Feishu WebSocket connected");

    std::thread heart([this] { heartbeat(); });
    heart.detach();

    while (running_ && connected_) {
        std::string msg;
        if (!ws.read(msg)) {
            LOG_WARN("WebSocket read error, reconnecting...");
            connected_ = false;
            break;
        }

        try {
            auto event = json::parse(msg);
            auto type = event.value("type", "");

            if (type == "im.message.receive_v1" && on_event_) {
                on_event_(event);
                LOG_DEBUG("Message event received");
            } else if (type == "ping") {
                std::string pong = R"({"type":"pong"})";
                ws.send(pong);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("WS parse error: {}", e.what());
        }
    }

    if (ws.is_valid()) ws.close();
    connected_ = false;
    return true;
}

void FeishuWSClient::heartbeat() {
    while (running_ && connected_) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        if (!token_.empty()) {
            auto new_token = get_tenant_token();
            if (!new_token.empty()) token_ = new_token;
        }
    }
}

void FeishuWSClient::run_loop() {
    int retry = 0;
    while (running_) {
        if (token_.empty() || retry > 0) {
            token_ = get_tenant_token();
            if (token_.empty()) {
                retry++;
                int delay = std::min(retry * 2, 30);
                LOG_WARN("Retry {} in {}s", retry, delay);
                std::this_thread::sleep_for(std::chrono::seconds(delay));
                continue;
            }
        }

        if (connect_ws()) {
            retry = 0;
        } else {
            retry++;
            LOG_WARN("WS reconnect attempt {}", retry);
        }
    }
}

} // namespace opencode
