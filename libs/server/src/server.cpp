#include "server.h"
#include "context_utils.h"
#include "tools/tools.h"
#include "plugin_tool.h"

#include <iostream>
#include <sys/socket.h>

namespace codis {

CodisServer::CodisServer(int port, std::optional<std::string> config_path)
    : port_(port)
    , server_(std::make_unique<httplib::Server>())
    , agent_loop_(hub_, session_store_, system_context_, tool_registry_, provider_registry_, config_)
    , ws_gateway_(hub_, session_store_, agent_loop_, provider_registry_, config_)
    , http_api_(HttpApi::Deps{
          .port = port_,
          .config = config_,
          .store = session_store_,
          .system = system_context_,
          .tools = tool_registry_,
          .providers = provider_registry_,
          .loop = agent_loop_,
          .balance = balance_client_,
          .skills = [this]() -> json {
              json sk = json::array();
              if (skill_tool_) {
                  for (auto& s : skill_tool_->available())
                      sk.push_back({{"id", s.id}, {"name", s.name}, {"description", s.description}});
              }
              return sk;
          },
          .mcp_status = [this]() -> json {
              return mcp_manager_ ? mcp_manager_->status() : json::array();
          },
      })
{
    // SSE 长连接不能因为空闲被断
    server_->set_keep_alive_timeout(0);

    // 默认 socket 选项用 SO_REUSEPORT，会让两个进程同端口共存并把新连接分流——
    // WS 长连接和 HTTP 请求被分到不同进程时消息会静默丢失。
    // 只保留 SO_REUSEADDR（允许 TIME_WAIT 重用），第二个实例 bind 时直接失败。
    server_->set_socket_options([](int sock) {
        int one = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    });

    if (config_path && !config_path->empty()) {
        config_ = AppConfig::load(*config_path);
    }

    for (auto& pc : config_.providers) {
        pc.resolve_api_key();
        if (!pc.api_key.empty()) provider_registry_.register_provider(pc, config_.proxy);
    }
    if (!config_.default_provider.empty())
        provider_registry_.set_default(config_.default_provider);

    if (provider_registry_.list().empty()) {
        const char* key = std::getenv("OPENAI_API_KEY");
        if (key) {
            ProviderConfig pc{"openai", key, "", "gpt-4o", "https://api.openai.com/v1"};
            provider_registry_.register_provider(pc, config_.proxy);
        }
        key = std::getenv("DEEPSEEK_API_KEY");
        if (key) {
            ProviderConfig pc{"deepseek", key, "", "deepseek-chat", "https://api.deepseek.com/v1"};
            provider_registry_.register_provider(pc, config_.proxy);
        }
    }

    // 注册默认工具
    tool_registry_.register_tool(std::make_unique<tools::ReadTool>());
    tool_registry_.register_tool(std::make_unique<tools::WriteTool>());
    tool_registry_.register_tool(std::make_unique<tools::EditTool>());
    tool_registry_.register_tool(std::make_unique<tools::BashTool>());
    tool_registry_.register_tool(std::make_unique<tools::GlobTool>());
    tool_registry_.register_tool(std::make_unique<tools::GrepTool>());
    tool_registry_.register_tool(std::make_unique<tools::WebSearchTool>(tools::WebSearchOptions{
        config_.websearch.backend, config_.websearch.api_key,
        config_.websearch.max_results, config_.websearch.timeout_seconds, config_.proxy}));
    auto skill_tool = std::make_unique<tools::SkillTool>(config_.skills.dirs);
    skill_tool_ = skill_tool.get();
    tool_registry_.register_tool(std::move(skill_tool));

    // [permissions] 策略覆盖工具默认权限（deny > allow > ask，均覆盖默认）
    for (auto& name : config_.permissions.allow)
        tool_registry_.set_permission(name, Permission::Allow);
    for (auto& name : config_.permissions.ask)
        tool_registry_.set_permission(name, Permission::Ask);
    for (auto& name : config_.permissions.deny)
        tool_registry_.set_permission(name, Permission::Denied);

    // 加载插件
    plugin_loader_.set_tool_registrar(
        [this](const std::string& name, const std::string& desc,
               const std::string& params, codis_tool_execute_fn exec, void* ctx) {
            json params_json;
            try { params_json = json::parse(params); } catch (...) { params_json = json::object(); }
            tool_registry_.register_tool(
                std::make_unique<PluginTool>(name, desc, params_json, exec, ctx));
        });
    plugin_loader_.set_logger([](int level, const std::string& msg) {
        LOG_INFO("plugin: {}", msg);
    });
    const char* plugin_dir = std::getenv("CODIS_PLUGIN_DIR");
    if (plugin_dir) plugin_loader_.load_directory(plugin_dir);
    else plugin_loader_.load_directory("plugins");

    // MCP 服务器：连接 + tools/list + 注册（std::move 转换配置结构）
    {
        std::vector<mcp::McpServerOptions> opts;
        for (auto& s : config_.mcp.servers) {
            mcp::McpServerOptions o;
            o.name = s.name;
            o.transport = s.transport;
            o.command = s.command;
            o.args = s.args;
            o.env = s.env;
            o.url = s.url;
            o.bearer_token = s.bearer_token;
            o.timeout_seconds = s.timeout_seconds;
            o.proxy = config_.proxy;
            opts.push_back(std::move(o));
        }
        mcp_manager_ = std::make_unique<mcp::McpManager>(std::move(opts), &tool_registry_);
        mcp_manager_->start_all();
    }

    init_context_sources();

    // WS/switch 路由（连接与帧分发层）
    server_->WebSocket(R"(/api/v1/acp/ws/([a-f0-9\-]+))",
                       [this](auto& r, auto& ws) { ws_gateway_.handle_ws(r, ws); });
    server_->Post("/api/v1/acp/switch",
                  [this](auto& r, auto& s) { ws_gateway_.handle_switch(r, s); });
    // REST 路由
    http_api_.register_routes(*server_);
}

// =============================================================================
// Context Sources 初始化
// =============================================================================

void CodisServer::init_context_sources() {
    system_context_.register_source(context_sources::date_source());
    system_context_.register_source(context_sources::working_dir_source());
    system_context_.register_source(context_sources::platform_source());
    system_context_.register_source(context_sources::git_status_source());
    system_context_.register_source(context_sources::project_instructions_source("."));
    system_context_.register_source(context_sources::tools_source([this]() {
        return tool_registry_.all_schemas();
    }));
}

CodisServer::~CodisServer() { stop(); }

void CodisServer::start() {
    running_ = true;
    thread_ = std::make_unique<std::thread>([this] {
        LOG_INFO("Server listening on http://localhost:{}", port_);
        for (auto& p : provider_registry_.list()) LOG_INFO("  provider: {}", p);
        for (auto& t : tool_registry_.list()) LOG_INFO("  tool: {}", t);
        LOG_INFO("  default provider: {}", provider_registry_.default_name());
        if (!server_->listen("127.0.0.1", port_)) {
            // bind 失败（端口被占用）：错误退出而非静默空转
            LOG_ERROR("Failed to bind port {} — address already in use? "
                      "Another codis-server may already be listening. Exiting.", port_);
            std::_Exit(1);
        }
    });
}

void CodisServer::stop() {
    if (running_.exchange(false)) { server_->stop(); if (thread_ && thread_->joinable()) thread_->join(); }
    if (mcp_manager_) mcp_manager_->stop_all();
    LOG_INFO("Server stopped");
}

} // namespace codis