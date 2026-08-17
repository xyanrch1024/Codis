#pragma once

#include "agent_loop.h"
#include "balance_client.h"
#include "config.h"
#include "context_source.h"
#include "http_api.h"
#include "mcp_manager.h"
#include "plugin_loader.h"
#include "provider_registry.h"
#include "session_hub.h"
#include "session_store.h"
#include "tool_registry.h"
#include "tools/skill_tool.h"
#include "ws_gateway.h"

#include <httplib.h>

#include <string>
#include <memory>
#include <optional>
#include <atomic>

namespace codis {

// =============================================================================
// CodisServer — 组合根：装配各组件并管理生命周期，不承载业务逻辑。
// 分层：HttpApi（REST）/ WsGateway（WS+帧分发）→ SessionHub（会话状态中枢）
// + AgentLoop（ACP 任务循环/压缩）→ SessionStore/SystemContext/各 Registry。
// =============================================================================

class CodisServer {
public:
    CodisServer(int port = 8711, std::optional<std::string> config_path = std::nullopt);
    ~CodisServer();

    void start();
    void stop();

    int port() const { return port_; }

private:
    void init_context_sources();

    int port_;
    std::unique_ptr<httplib::Server> server_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool> running_{false};

    ProviderRegistry provider_registry_;
    ToolRegistry tool_registry_;
    tools::SkillTool* skill_tool_ = nullptr;
    AppConfig config_;
    SessionStore session_store_{"/tmp/codis_sessions.db"};
    SystemContext system_context_;
    PluginLoader plugin_loader_;
    std::unique_ptr<mcp::McpManager> mcp_manager_;

    SessionHub hub_;
    AgentLoop agent_loop_;
    WsGateway ws_gateway_;
    BalanceClient balance_client_;
    HttpApi http_api_;
};

} // namespace codis