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
#include <httplib.h>

namespace opencode {

struct SessionInfo {
    std::string id;
    std::string title;
    int message_count = 0;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    std::vector<Message> messages;
};

class AcpClient {
public:
    // 事件回调
    using AssistantCallback = std::function<void(std::string_view delta)>;
    using ToolCallCallback  = std::function<void(const acp::ToolCallEvent&)>;
    using ToolResultCallback= std::function<void(const acp::ToolResultEvent&)>;
    using ErrorCallback     = std::function<void(std::string_view message)>;
    using DoneCallback      = std::function<void()>;

    struct Callbacks {
        AssistantCallback   on_assistant;
        ToolCallCallback    on_tool_call;
        ToolResultCallback  on_tool_result;
        ErrorCallback       on_error;
        DoneCallback        on_done;
    };

    AcpClient(int server_port = 8711);

    // 发起 ACP 对话（同步阻塞直到完成）
    bool send(const ChatRequest& request, Callbacks callbacks);

    // fire-and-forget: 只发送消息，不等待回复
    bool send_async(const ChatRequest& request);

    // 长连接模式：打开 SSE 流，后台持续回调
    bool connect(const std::string& session_id, Callbacks callbacks);
    void disconnect();

    // 健康检查
    bool health_check();

    // 会话管理
    std::optional<std::string> create_session();
    std::vector<SessionInfo> list_sessions();
    std::optional<SessionInfo> get_session(const std::string& id);
    bool delete_session(const std::string& id);
    std::string get_last_session();

private:
    void parse_and_dispatch(const std::string& sse_data, Callbacks& cbs);

    std::string host_;
    int port_;
    std::unique_ptr<httplib::Client> http_;
    std::thread sse_thread_;
    std::atomic<bool> connected_{false};
    Callbacks callbacks_;
};

} // namespace opencode
