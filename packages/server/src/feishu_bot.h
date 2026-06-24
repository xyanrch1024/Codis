#pragma once

#include "feishu_ws_client.h"
#include "types.h"
#include "config.h"

#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <thread>

namespace opencode {

class AcpClient;

class FeishuBot {
public:
    FeishuBot(const std::string& app_id, const std::string& app_secret);
    ~FeishuBot();

    void start();
    void stop();

private:
    void on_fs_message(const json& event);
    void send_reply(const std::string& msg_id, const std::string& text);
    std::string get_or_create_session(const std::string& chat_id);

    FeishuWSClient ws_;
    std::unordered_map<std::string, std::string> chat_sessions_;  // chat_id → session_id
    std::mutex sessions_mutex_;
    std::string token_;
};

} // namespace opencode
