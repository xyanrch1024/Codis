#pragma once

#include "acp.h"
#include "log.h"
#include "types.h"

#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <map>
#include <vector>
#include <httplib.h>

namespace codis {

struct SessionInfo {
    std::string id;
    std::string title;
    int message_count = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::vector<Message> messages;
};

struct ServerInfo {
    std::vector<std::string> providers;
    std::string default_provider;
    std::map<std::string, std::string> provider_models;  // name -> model
};

class AcpClient {
public:
    // 事件回调
    using AssistantCallback = std::function<void(std::string_view delta)>;
    using ReasoningCallback = std::function<void(std::string_view delta)>;
    using ToolCallCallback  = std::function<void(const acp::ToolCallEvent&)>;
    using ToolResultCallback= std::function<void(const acp::ToolResultEvent&)>;
    using ErrorCallback     = std::function<void(std::string_view message)>;
    using DoneCallback      = std::function<void()>;

    struct Callbacks {
        AssistantCallback   on_assistant;
        ReasoningCallback   on_reasoning;
        ToolCallCallback    on_tool_call;
        ToolResultCallback  on_tool_result;
        ErrorCallback       on_error;
        DoneCallback        on_done;
    };

    AcpClient(int server_port = 8711);

    // fire-and-forget: 通过 WS 全双工发送消息，不等待回复
    bool send_async(const ChatRequest& request);

    // 长连接模式：打开 WebSocket 流，后台持续回调
    bool connect(const std::string& session_id, Callbacks callbacks);
    void disconnect();
    // 健康检查
    bool health_check();

    // 会话管理
    std::optional<std::string> create_session();
    std::vector<SessionInfo> list_sessions();
    std::optional<SessionInfo> get_session(const std::string& id);
    bool delete_session(const std::string& id);
    bool delete_all_sessions();
    bool switch_session(const std::string& session_id);
    void cancel_session(const std::string& session_id);
    std::string get_last_session();

    // 服务器信息（providers / models / 特性）
    std::optional<ServerInfo> get_server_info();

    // WebSocket 长连接是否已建立
    bool connected() const { return connected_.load(); }

private:
    // WS 就绪前/断线期间的待发请求，connect 成功后 flush
    void flush_pending();

    std::string host_;
    int port_;
    std::unique_ptr<httplib::Client> http_;
    std::thread sse_thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> thread_done_{true};
    std::mutex ws_mutex_;
    std::unique_ptr<httplib::ws::WebSocketClient> ws_;
    Callbacks callbacks_;
    std::string conn_id_;
    std::mutex pending_mutex_;
    std::deque<std::string> pending_outbound_;
};

} // namespace codis
