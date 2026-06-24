#pragma once

#include "types.h"
#include "acp.h"
#include "provider.h"
#include "config.h"
#include "provider_registry.h"
#include "tool_registry.h"
#include "session_store.h"
#include "context_source.h"

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
// Session Manager (Legacy — 保留兼容, 新代码用 SessionStore)
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
};

// =============================================================================
// Active Session — 活跃会话（多 client 共享）
// =============================================================================

struct ActiveSession {
    std::vector<std::weak_ptr<SseFrameQueue>> clients;  // 所有监听者
    std::atomic<bool> processing{false};
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

    // session → attach client 或创建新的
    std::shared_ptr<SseFrameQueue> attach_to_session(
        const std::string& session_id, ChatRequest& req, bool& is_new);

    void handle_health(const httplib::Request& req, httplib::Response& res);
    void handle_info(const httplib::Request& req, httplib::Response& res);
    void handle_chat(const httplib::Request& req, httplib::Response& res);
    void handle_acp(const httplib::Request& req, httplib::Response& res);
    void handle_session_create(const httplib::Request& req, httplib::Response& res);
    void handle_session_list(const httplib::Request& req, httplib::Response& res);
    void handle_session_get(const httplib::Request& req, httplib::Response& res);
    void handle_session_delete(const httplib::Request& req, httplib::Response& res);
    void handle_session_add_message(const httplib::Request& req, httplib::Response& res);

    std::string call_llm(const ChatRequest& req, const json& tools);
    void call_llm_stream(const ChatRequest& req, SseFrameQueue& frames, const json& tools);

    std::shared_ptr<LLMProvider> resolve_provider(const ChatRequest& req);
    std::vector<ToolCall> extract_tool_calls(const std::string& content);

    // 改造为广播版本
    void run_acp_loop_broadcast(const std::string& session_id, ChatRequest req);

    void init_context_sources();
    std::string build_system_prompt(const std::string& session_id);

    int port_;
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};

    SessionManager session_mgr_;
    ProviderRegistry provider_registry_;
    ToolRegistry tool_registry_;
    AppConfig config_;
    SessionStore session_store_{"/tmp/codis_sessions.db"};
    SystemContext system_context_;

    std::shared_mutex active_mutex_;
    std::unordered_map<std::string, ActiveSession> active_sessions_;
};

} // namespace opencode
