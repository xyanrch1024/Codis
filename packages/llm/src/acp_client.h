#pragma once

#include "acp.h"
#include "log.h"
#include "types.h"

#include <string>
#include <functional>
#include <memory>
#include <optional>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <httplib.h>

namespace opencode {

struct SessionInfo {
    std::string id;
    std::string model;
    std::string provider;
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

    // 健康检查
    bool health_check();

    // 会话管理（复用现有 REST 端点）
    std::optional<std::string> create_session();
    std::optional<SessionInfo> get_session(const std::string& id);

private:
    void parse_and_dispatch(const std::string& sse_data, Callbacks& cbs);

    std::string host_;
    int port_;
    std::unique_ptr<httplib::Client> http_;
};

} // namespace opencode
