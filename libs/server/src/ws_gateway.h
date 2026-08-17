// WsGateway — ACP WebSocket 连接生命周期 + 帧分发。
// 职责：WS 建连/断线重连身份迁移、发送线程、读循环、ACP request/cancel/
// confirm_ack/switch/compact 帧处理、REST /api/v1/acp/switch 端点。

#pragma once

#include "agent_loop.h"
#include "acp.h"
#include "config.h"
#include "provider_registry.h"
#include "session_hub.h"
#include "session_store.h"

#include <httplib.h>

namespace codis {

class WsGateway {
public:
    WsGateway(SessionHub& hub, SessionStore& store, AgentLoop& loop,
              ProviderRegistry& providers, const AppConfig& config);

    void handle_ws(const httplib::Request& req, httplib::ws::WebSocket& ws);
    void handle_switch(const httplib::Request& req, httplib::Response& res);

private:
    // 处理单条 WS 事件；抛异常由调用方转 error_frame 回送
    void dispatch_frame(const acp::ParsedEvent& event, const std::string& sid,
                        const std::string& conn_id, httplib::ws::WebSocket& ws);

    SessionHub& hub_;
    SessionStore& store_;
    AgentLoop& loop_;
    ProviderRegistry& providers_;
    const AppConfig& config_;
};

} // namespace codis
