#pragma once

#include "acp.h"
#include "log.h"
#include "messages.h"

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

struct HttpResult {
    bool ok = false;          // HTTP 请求成功返回（任意状态码）
    int status = 0;           // 状态码（ok=true 时有效）
    std::string body;         // 响应体
    std::string error;        // ok=false 时的传输错误描述
};

struct SessionInfo {
    std::string id;
    std::string title;
    int message_count = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::vector<Message> messages;
};

struct SkillBrief {
    std::string id;
    std::string name;
    std::string description;
};

struct McpServerBrief {
    std::string name;
    std::string transport;   // stdio | http
    bool online = false;
    int tool_count = 0;
};

struct ServerInfo {
    std::vector<std::string> providers;
    std::string default_provider;
    std::map<std::string, std::string> provider_models;  // name -> model
    std::vector<SkillBrief> skills;          // 已安装技能
    std::vector<McpServerBrief> mcp_servers; // MCP 服务器状态
};

class AcpClient {
public:
    // 事件回调
    using AssistantCallback = std::function<void(std::string_view delta)>;
    using ReasoningCallback = std::function<void(std::string_view delta)>;
    using ToolCallCallback  = std::function<void(const acp::ToolCallEvent&)>;
    using ToolResultCallback= std::function<void(const acp::ToolResultEvent&)>;
    using ToolConfirmCallback = std::function<void(const std::string& confirm_id,
                                                   const acp::ToolCallEvent& call,
                                                   int timeout_seconds)>;
    using ErrorCallback     = std::function<void(std::string_view message)>;
    using DoneCallback      = std::function<void()>;
    using CompactedCallback = std::function<void(const acp::CompactResultEvent&)>;
    using ContextStatsCallback = std::function<void(const acp::ContextStatsEvent&)>;
    // 连接状态变化（true=WS 在线，false=断线/重连中），供 UI 刷新状态栏
    using ConnectionCallback = std::function<void(bool online)>;

    struct Callbacks {
        AssistantCallback   on_assistant;
        ReasoningCallback   on_reasoning;
        ToolCallCallback    on_tool_call;
        ToolResultCallback  on_tool_result;
        ToolConfirmCallback on_tool_confirm;
        ErrorCallback       on_error;
        DoneCallback        on_done;
        CompactedCallback   on_compacted;
        ContextStatsCallback on_context_stats;
        ConnectionCallback  on_connection;
    };

    AcpClient(int server_port = 8711);
    virtual ~AcpClient() = default;

    // fire-and-forget: 通过 WS 全双工发送消息，不等待回复
    virtual bool send_async(const ChatRequest& request);

    // 工具确认回执：approved=true 批准，false 拒绝（WS 未就绪时入待发队列）
    virtual void send_confirmation(const std::string& confirm_id, bool approved);

    // 长连接模式：打开 WebSocket 流，后台持续回调
    virtual bool connect(const std::string& session_id, Callbacks callbacks);
    virtual void disconnect();
    // 健康检查
    virtual bool health_check();

    // 会话管理
    virtual std::optional<std::string> create_session();
    virtual std::vector<SessionInfo> list_sessions();
    virtual std::optional<SessionInfo> get_session(const std::string& id);
    virtual bool delete_session(const std::string& id);
    virtual bool delete_all_sessions();
    virtual bool switch_session(const std::string& session_id);
    virtual void cancel_session(const std::string& session_id);
    // 请求上下文压缩（WS 发送 compact 帧；keep = 保留尾部原文条数）
    virtual void send_compact(const std::string& session_id, int keep = 20);
    virtual std::string get_last_session();

    // 服务器信息（providers / models / 特性）
    virtual std::optional<ServerInfo> get_server_info();

    // 通用 GET（cmd_balance 等业务查询走这里，便于测试替身替换）
    virtual HttpResult http_get(const std::string& path);

    // WebSocket 长连接是否已建立
    bool connected() const { return ws_online_.load(); }

private:
    // WS 就绪前/断线期间的待发请求，connect 成功后 flush
    void flush_pending();

    std::string host_;
    int port_;
    std::unique_ptr<httplib::Client> http_;
    std::thread sse_thread_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> ws_online_{false};   // WS 实际在线状态（断线置 false，重连成功置 true）
    std::atomic<bool> thread_done_{true};
    std::mutex ws_mutex_;
    std::unique_ptr<httplib::ws::WebSocketClient> ws_;
    Callbacks callbacks_;
    std::string conn_id_;
    // 当前会话：connect 时初始化，switch_session 时更新。
    // 重连循环必须用它（而非 connect 时的初始值）拼 URL——
    // 否则切换会话后断线重连会把 conn 从新会话迁回旧会话，任务广播全部 drop。
    std::string current_session_;
    std::mutex pending_mutex_;
    std::deque<std::string> pending_outbound_;
};

} // namespace codis
