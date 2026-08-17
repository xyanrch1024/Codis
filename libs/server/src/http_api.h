// HttpApi — REST 路由与处理器（health/info/chat/sessions CRUD/balance）。
// 纯分发层：业务逻辑经依赖注入的组件访问，自身不持有会话状态。

#pragma once

#include "agent_loop.h"
#include "balance_client.h"
#include "config.h"
#include "context_source.h"
#include "provider_registry.h"
#include "session_store.h"
#include "tool_registry.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <functional>

namespace codis {

using json = nlohmann::json;

class HttpApi {
public:
    struct Deps {
        int port = 8711;
        const AppConfig& config;
        SessionStore& store;
        SystemContext& system;
        ToolRegistry& tools;
        ProviderRegistry& providers;
        AgentLoop& loop;
        BalanceClient& balance;
        // 惰性求值（装配期组件可能尚未就绪）
        std::function<json()> skills;      // /api/v1/info 的已安装技能列表
        std::function<json()> mcp_status;  // /api/v1/info 的 MCP 服务器状态
    };

    explicit HttpApi(Deps deps);
    void register_routes(httplib::Server& srv);

private:
    static void set_cors(httplib::Response& res);

    void handle_health(const httplib::Request& req, httplib::Response& res);
    void handle_info(const httplib::Request& req, httplib::Response& res);
    void handle_chat(const httplib::Request& req, httplib::Response& res);
    void handle_balance(const httplib::Request& req, httplib::Response& res);
    void handle_session_create(const httplib::Request& req, httplib::Response& res);
    void handle_session_list(const httplib::Request& req, httplib::Response& res);
    void handle_session_get(const httplib::Request& req, httplib::Response& res);
    void handle_session_delete(const httplib::Request& req, httplib::Response& res);
    void handle_session_delete_all(const httplib::Request& req, httplib::Response& res);
    void handle_session_add_message(const httplib::Request& req, httplib::Response& res);

    Deps deps_;
};

} // namespace codis