#pragma once

#include "types.h"
#include "acp.h"
#include "provider.h"
#include "config.h"
#include "provider_registry.h"
#include "tool_registry.h"
#include "session_store.h"
#include "context_source.h"
#include "plugin_loader.h"
#include "plugin_tool.h"

#include <httplib.h>

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <format>
#include <shared_mutex>
#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace codis {

// =============================================================================
// 每连接帧缓冲队列（ACP 循环 push，WS 发送线程 pop）
// =============================================================================

class FrameQueue {
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
// Session State — 每个 session 的 connections + processing 状态
// =============================================================================

struct SessionState {
    std::unordered_map<std::string, std::shared_ptr<FrameQueue>> conns;
    std::mutex mutex;
    std::atomic<bool> processing{false};
    // 客户端取消当前任务 — 用 shared_ptr 持有，保证 run 线程在 session 条目被
    // 清除后仍能安全读写该标志
    std::shared_ptr<std::atomic<bool>> cancel_requested{std::make_shared<std::atomic<bool>>(false)};
    std::deque<ChatRequest> pending;  // 处理期间到达的请求，当前轮结束后按序补跑
};

// =============================================================================
// HTTP Server
// =============================================================================

class CodisServer {
public:
    CodisServer(int port = 8711, std::optional<std::string> config_path = std::nullopt);
    ~CodisServer();

    void start();
    void stop();

    int port() const { return port_; }

private:
    void register_routes();
    void set_cors(httplib::Response& res);

    void handle_health(const httplib::Request& req, httplib::Response& res);
    void handle_info(const httplib::Request& req, httplib::Response& res);
    void handle_chat(const httplib::Request& req, httplib::Response& res);
    void handle_acp_ws(const httplib::Request& req, httplib::ws::WebSocket& ws);
    void handle_acp_switch(const httplib::Request& req, httplib::Response& res);
    void handle_balance(const httplib::Request& req, httplib::Response& res);
    void handle_session_create(const httplib::Request& req, httplib::Response& res);
    void handle_session_list(const httplib::Request& req, httplib::Response& res);
    void handle_session_get(const httplib::Request& req, httplib::Response& res);
    void handle_session_delete(const httplib::Request& req, httplib::Response& res);
    void handle_session_delete_all(const httplib::Request& req, httplib::Response& res);
    void handle_session_add_message(const httplib::Request& req, httplib::Response& res);
    std::string call_llm(const ChatRequest& req);
    json query_provider_balance(const std::string& provider_name);

    std::shared_ptr<LLMProvider> resolve_provider(const ChatRequest& req);
    std::vector<ToolCall> extract_tool_calls(const std::string& content);

    // 全双工入口：追加 user 消息 → processing/pending 检查 → 启动 ACP 循环
    void queue_chat_request(const std::string& session_id,
                            const std::string& conn_id, ChatRequest req);
    // 把 conn_id 从当前 session 移到目标 session，返回是否成功
    bool move_connection(const std::string& conn_id, const std::string& new_sid);
    void run_acp_loop_broadcast(const std::string& session_id,
                                 const std::string& conn_id, ChatRequest req);
    void cleanup_connection(const std::string& session_id, const std::string& conn_id);
    std::string generate_conn_id();

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
    std::unordered_map<std::string, SessionState> sessions_;
    std::mutex sessions_mutex_;
    PluginLoader plugin_loader_;
};

} // namespace codis
