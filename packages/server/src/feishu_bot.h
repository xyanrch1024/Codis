#pragma once

#include "types.h"

#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>

namespace opencode {

class FeishuBot {
public:
    FeishuBot(const std::string& app_id, const std::string& app_secret);
    ~FeishuBot();

    void start();
    void stop();

private:
    void on_message(const json& event);
    void poll_loop();                           // HTTP 轮询
    std::string get_or_create_session(const std::string& chat_id);
    std::string get_tenant_token();
    void send_reply(const std::string& msg_id, const std::string& text);
    void send_message(const std::string& chat_id, const std::string& text);

    std::string app_id_;
    std::string app_secret_;
    std::string token_;
    int64_t token_expire_ = 0;
    std::map<std::string, std::string> chat_sessions_;
    std::map<std::string, std::string> last_msg_;  // dedup
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> running_{false};

    friend class OpenCodeServer;
};

} // namespace opencode
