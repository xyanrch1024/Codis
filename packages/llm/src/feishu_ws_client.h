#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <memory>
#include <nlohmann/json.hpp>

namespace opencode {

using json = nlohmann::json;

class FeishuWSClient {
public:
    using EventCallback = std::function<void(const json& event)>;

    FeishuWSClient(const std::string& app_id, const std::string& app_secret);
    ~FeishuWSClient();

    void set_event_callback(EventCallback cb) { on_event_ = std::move(cb); }
    void start();
    void stop();
    bool is_connected() const { return connected_; }

private:
    void run_loop();
    std::string get_tenant_token();
    bool connect_ws();
    void heartbeat();
    std::string app_id_;
    std::string app_secret_;
    std::string token_;
    EventCallback on_event_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
};

} // namespace opencode
