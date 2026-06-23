#pragma once

#include "types.h"
#include "acp.h"
#include "provider.h"
#include "config.h"
#include "provider_registry.h"

#include <httplib.h>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <shared_mutex>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace opencode {

// =============================================================================
// SSE 帧缓冲队列
// =============================================================================

class SseFrameQueue {
public:
    void push(std::string frame);
    std::string pop();
    void close();

private:
    std::queue<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> closed_{false};
};

// =============================================================================
// Session Data
// =============================================================================

struct SessionData {
    std::string id;
    std::vector<Message> messages;
    std::string model = "gpt-4o";
    std::string provider = "openai";
};

// =============================================================================
// Session Manager
// =============================================================================

class SessionManager {
public:
    std::string create_session();
    std::optional<SessionData> get_session(const std::string& id);
    void add_message(const std::string& id, const Message& msg);
    std::vector<std::string> list_sessions() const;

private:
    std::string next_id();
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, SessionData> sessions_;
    uint64_t counter_ = 0;
};

// =============================================================================
// HTTP Server
// =============================================================================

class OpenCodeServer {
public:
    OpenCodeServer(int port = 8711, std::optional<std::string> config_path = std::nullopt);
    ~OpenCodeServer();

    void start();
    void stop();

    int port() const { return port_; }

private:
    void register_routes();
    void set_cors(httplib::Response& res);

    void handle_health(const httplib::Request& req, httplib::Response& res);
    void handle_info(const httplib::Request& req, httplib::Response& res);
    void handle_chat(const httplib::Request& req, httplib::Response& res);
    void handle_acp(const httplib::Request& req, httplib::Response& res);
    void handle_session_create(const httplib::Request& req, httplib::Response& res);
    void handle_session_get(const httplib::Request& req, httplib::Response& res);
    void handle_session_add_message(const httplib::Request& req, httplib::Response& res);

    std::string call_llm(const ChatRequest& req);
    void call_llm_stream(const ChatRequest& req, SseFrameQueue& frames);

    std::shared_ptr<LLMProvider> resolve_provider(const ChatRequest& req);

    int port_;
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};

    SessionManager session_mgr_;
    ProviderRegistry provider_registry_;
    AppConfig config_;
};

} // namespace opencode
