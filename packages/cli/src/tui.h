#pragma once

#include "acp_client.h"
#include "types.h"

#include <string>
#include <vector>
#include <mutex>
#include <functional>
#include <memory>

namespace opencode {

struct TuiState {
    std::vector<std::string> lines;
    std::string pending;
    std::string current_session;
    std::string model;
    int server_port = 8711;
    std::string system_prompt = "You are a helpful AI coding assistant.";
    bool processing = false;
    std::vector<Message> history;
    std::string status_msg;

    std::mutex mutex;

    // 由 TuiClient::run() 设置，指向 screen.Post
    std::function<void()> notify_;

    void add_line(const std::string& line) {
        std::lock_guard lk(mutex);
        lines.push_back(line);
        if (notify_) notify_();
    }
    void append_pending(const std::string& delta) {
        std::lock_guard lk(mutex);
        pending += delta;
        if (notify_) notify_();
    }
    void flush_pending() {
        std::lock_guard lk(mutex);
        if (!pending.empty()) {
            lines.push_back("AI: " + pending);
            history.push_back({"assistant", pending});
            pending.clear();
        }
        processing = false;
        if (notify_) notify_();
    }
};

class TuiClient {
public:
    TuiClient(int server_port, std::string model, std::string provider,
              std::string session_arg);
    int run();

private:
    void send_message(const std::string& text);
    void cmd_clear();
    void cmd_delete_all();
    void cmd_balance(const std::string& line);

    int server_port_;
    std::string model_;
    std::string provider_;
    std::string session_arg_;
    AcpClient acp_;
    std::shared_ptr<TuiState> state_;
    std::function<void()> post_job_;

    // Session overlay
    bool sessions_visible_ = false;
    int session_selected_ = 0;
    std::vector<SessionInfo> session_list_;
    void switch_session(const SessionInfo& s);
    void connect_sse();
    AcpClient::Callbacks build_callbacks();
};

} // namespace opencode
