#include "server.h"
#include "tools/tools.h"

#include <iostream>
#include <thread>
#include <random>
#include <sstream>
#include <iomanip>

namespace opencode {

// =============================================================================
// SseFrameQueue
// =============================================================================

void SseFrameQueue::push(std::string frame) {
    { std::lock_guard lock(mutex_); queue_.push(std::move(frame)); }
    cv_.notify_one();
}
std::string SseFrameQueue::pop() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return "";
    auto frame = std::move(queue_.front()); queue_.pop();
    return frame;
}
void SseFrameQueue::close() { closed_ = true; cv_.notify_all(); }

// =============================================================================
// SessionManager
// =============================================================================

std::string SessionManager::next_id() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15), dis2(8, 11);
    std::ostringstream ss; ss << std::hex << std::setfill('0');
    for (int i = 0; i < 8; i++)  ss << dis(gen); ss << "-";
    for (int i = 0; i < 4; i++)  ss << dis(gen); ss << "-4";
    for (int i = 0; i < 3; i++)  ss << dis(gen); ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++)  ss << dis(gen); ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);
    return ss.str();
}
std::string SessionManager::create_session() {
    std::unique_lock lock(mutex_);
    auto id = next_id(); sessions_[id] = {.id = id};
    return id;
}
std::optional<SessionData> SessionManager::get_session(const std::string& id) {
    std::shared_lock lock(mutex_);
    auto it = sessions_.find(id);
    return it != sessions_.end() ? std::optional(it->second) : std::nullopt;
}
void SessionManager::add_message(const std::string& id, const Message& msg) {
    std::unique_lock lock(mutex_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) it->second.messages.push_back(msg);
}
std::vector<std::string> SessionManager::list_sessions() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> ids;
    for (auto& [id, _] : sessions_) ids.push_back(id);
    return ids;
}

// =============================================================================
// OpenCodeServer
// =============================================================================

OpenCodeServer::OpenCodeServer(int port, std::optional<std::string> config_path)
    : port_(port)
    , server_(std::make_unique<httplib::Server>())
{
    if (config_path && !config_path->empty()) {
        config_ = AppConfig::load(*config_path);
    }

    for (auto& pc : config_.providers) {
        pc.resolve_api_key();
        if (!pc.api_key.empty()) provider_registry_.register_provider(pc);
    }
    if (!config_.default_provider.empty())
        provider_registry_.set_default(config_.default_provider);

    if (provider_registry_.list().empty()) {
        const char* key = std::getenv("OPENAI_API_KEY");
        if (key) {
            ProviderConfig pc{"openai", key, "", "gpt-4o", "https://api.openai.com/v1"};
            provider_registry_.register_provider(pc);
        }
        key = std::getenv("DEEPSEEK_API_KEY");
        if (key) {
            ProviderConfig pc{"deepseek", key, "", "deepseek-chat", "https://api.deepseek.com/v1"};
            provider_registry_.register_provider(pc);
        }
    }

    // 注册默认工具
    tool_registry_.register_tool(std::make_unique<tools::ReadTool>());
    tool_registry_.register_tool(std::make_unique<tools::WriteTool>());
    tool_registry_.register_tool(std::make_unique<tools::EditTool>());
    tool_registry_.register_tool(std::make_unique<tools::BashTool>());
    tool_registry_.register_tool(std::make_unique<tools::GlobTool>());
    tool_registry_.register_tool(std::make_unique<tools::GrepTool>());

    register_routes();
}

OpenCodeServer::~OpenCodeServer() { stop(); }

void OpenCodeServer::start() {
    running_ = true;
    thread_ = std::make_unique<std::thread>([this] {
        LOG_INFO("Server listening on http://localhost:{}", port_);
        for (auto& p : provider_registry_.list()) LOG_INFO("  provider: {}", p);
        for (auto& t : tool_registry_.list()) LOG_INFO("  tool: {}", t);
        LOG_INFO("  default provider: {}", provider_registry_.default_name());
        server_->listen("127.0.0.1", port_);
    });
}
void OpenCodeServer::stop() {
    if (running_.exchange(false)) { server_->stop(); if (thread_ && thread_->joinable()) thread_->join(); }
    LOG_INFO("Server stopped");
}

void OpenCodeServer::set_cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

// =============================================================================
// 路由
// =============================================================================

void OpenCodeServer::register_routes() {
    server_->Options("/api/v1/.*", [](const auto&, auto& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        res.status = 204;
    });
    server_->Get("/api/v1/health",       [this](auto& r, auto& s) { handle_health(r, s); });
    server_->Get("/api/v1/info",         [this](auto& r, auto& s) { handle_info(r, s); });
    server_->Post("/api/v1/chat",        [this](auto& r, auto& s) { handle_chat(r, s); });
    server_->Post("/api/v1/acp",         [this](auto& r, auto& s) { handle_acp(r, s); });
    server_->Post("/api/v1/sessions",    [this](auto& r, auto& s) { handle_session_create(r, s); });
    server_->Get(R"(/api/v1/sessions/([a-f0-9\-]+))",     [this](auto& r, auto& s) { handle_session_get(r, s); });
    server_->Post(R"(/api/v1/sessions/([a-f0-9\-]+)/messages)", [this](auto& r, auto& s) { handle_session_add_message(r, s); });
}

// =============================================================================
// 端点
// =============================================================================

void OpenCodeServer::handle_health(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    json j;
    j["status"] = "ok";
    j["version"] = "0.4.0";
    j["protocol"] = "acp";
    j["port"] = port_;
    j["default_provider"] = provider_registry_.default_name();
    j["tools"] = tool_registry_.list();
    res.set_content(j.dump(2), "application/json");
}

void OpenCodeServer::handle_info(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    json j;
    j["providers"] = provider_registry_.list();
    j["default_provider"] = provider_registry_.default_name();
    j["tools"] = tool_registry_.list();
    j["features"] = {"acp", "chat", "stream", "tools", "sessions"};
    res.set_content(j.dump(2), "application/json");
}

void OpenCodeServer::handle_chat(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto body = json::parse(req.body);
        auto chat_req = ChatRequest::from_json(body);

        json tools = json::array();
        for (auto& s : tool_registry_.all_schemas()) {
            tools.push_back({{"type", "function"}, {"function", {
                {"name", s.name},
                {"description", s.description},
                {"parameters", s.parameters}
            }}});
        }

        std::string result = call_llm(chat_req, tools);

        json resp;
        resp["content"] = result;
        resp["model"] = chat_req.model;
        resp["success"] = true;
        res.set_content(resp.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void OpenCodeServer::handle_acp(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto body = json::parse(req.body);
        auto chat_req = ChatRequest::from_json(body);
        chat_req.stream = true;

        auto frames = std::make_shared<SseFrameQueue>();

        std::thread llm_thread([this, req = std::move(chat_req), frames]() mutable {
            try {
                run_acp_loop(*frames, std::move(req));
                frames->push(acp::done_frame());
            } catch (const std::exception& e) {
                frames->push(acp::error_frame(e.what()));
                frames->push(acp::done_frame());
            }
            frames->close();
        });
        llm_thread.detach();

        res.set_chunked_content_provider("text/event-stream",
            [frames](size_t, httplib::DataSink& sink) -> bool {
                auto frame = frames->pop();
                if (frame.empty()) { sink.done(); return false; }
                if (!sink.write(frame.data(), frame.size())) return false;
                if (frame.find("\"done\"") != std::string::npos) { sink.done(); return false; }
                return true;
            });

    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

// =============================================================================
// ACP Loop — 多轮 tool call
// =============================================================================

void OpenCodeServer::run_acp_loop(SseFrameQueue& frames, ChatRequest req) {
    static const int MAX_TURNS = 10;

    json tools = json::array();
    for (auto& s : tool_registry_.all_schemas()) {
        tools.push_back({{"type", "function"}, {"function", {
            {"name", s.name},
            {"description", s.description},
            {"parameters", s.parameters}
        }}});
    }

    std::string assistant_content;
    auto turn = std::make_shared<int>(0);

    while (*turn < MAX_TURNS) {
        (*turn)++;
        LOG_DEBUG("ACP loop turn {}/{}", *turn, MAX_TURNS);

        // 调用 LLM
        assistant_content.clear();
        call_llm_stream(req, frames, tools);

        // 检查 tool calls
        auto call_list = extract_tool_calls(assistant_content);
        if (call_list.empty()) break;

        // 执行工具
        for (auto& call : call_list) {
            auto perm = tool_registry_.check_permission(call.name);
            if (perm == Permission::Denied) {
                frames.push(acp::tool_result_frame(call.id, false, "Permission denied"));
                continue;
            }
            auto result = tool_registry_.execute(call);
            frames.push(acp::tool_result_frame(result.id, result.success, result.content));

            Message asst_msg;
            asst_msg.role = "assistant";
            asst_msg.tool_call_id = call.id;
            asst_msg.tool_name = call.name;
            req.messages.push_back(asst_msg);

            Message tool_msg;
            tool_msg.role = "tool";
            tool_msg.content = result.content;
            tool_msg.tool_call_id = call.id;
            req.messages.push_back(tool_msg);
        }
    }

    if (*turn >= MAX_TURNS) {
        frames.push(acp::error_frame("Max turns reached"));
    }
}

// =============================================================================
// Tool call 提取 — 从 token 流中解析
// =============================================================================

std::vector<ToolCall> OpenCodeServer::extract_tool_calls(const std::string& content) {
    // 简单解析: 查找 LLM 输出的 JSON tool_call 块
    // 完整实现可匹配 ```json...``` 块或直接 JSON 数组
    std::vector<ToolCall> calls;
    auto pos = content.find("\"tool_calls\"");
    if (pos == std::string::npos) return calls;

    try {
        // 尝试解析为完整 JSON
        auto j = json::parse(content);
        if (j.contains("tool_calls")) {
            for (auto& tc : j["tool_calls"]) {
                ToolCall call;
                call.id = tc.value("id", "");
                auto& func = tc["function"];
                call.name = func.value("name", "");
                call.arguments = func.value("arguments", json::object());
                calls.push_back(call);
            }
        }
    } catch (...) {
        // 非 JSON 响应，无 tool calls
    }
    return calls;
}

// =============================================================================
// 会话端点
// =============================================================================

void OpenCodeServer::handle_session_create(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    auto id = session_mgr_.create_session();
    res.status = 201;
    res.set_content(json{{"session_id", id}}.dump(), "application/json");
}
void OpenCodeServer::handle_session_get(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    auto session = session_mgr_.get_session(req.matches[1]);
    if (!session) { res.status = 404; res.set_content(R"({"error":"not found"})", "application/json"); return; }
    json j;
    j["id"] = session->id; j["model"] = session->model; j["provider"] = session->provider;
    j["messages"] = json::array();
    for (auto& m : session->messages) j["messages"].push_back(m.to_json());
    res.set_content(j.dump(2), "application/json");
}
void OpenCodeServer::handle_session_add_message(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto msg = Message::from_json(json::parse(req.body));
        session_mgr_.add_message(req.matches[1], msg);
        res.set_content(R"({"status":"ok"})", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

// =============================================================================
// Provider 解析 + LLM 调用
// =============================================================================

std::shared_ptr<LLMProvider> OpenCodeServer::resolve_provider(const ChatRequest& req) {
    std::string name = req.provider.empty() ? provider_registry_.default_name() : req.provider;
    LOG_DEBUG("resolving provider '{}'", name);
    auto provider = provider_registry_.get(name);
    if (provider) return *provider;
    LOG_WARN("provider '{}' not found, fallback", name);
    auto list = provider_registry_.list();
    if (!list.empty()) return *provider_registry_.get(list[0]);
    throw std::runtime_error("No provider configured");
}

std::string OpenCodeServer::call_llm(const ChatRequest& req, const json& tools) {
    auto prov = resolve_provider(req);
    auto result = prov->chat(req);
    if (!result.success) {
        LOG_ERROR("LLM call failed: {}", result.error);
        throw std::runtime_error(result.error);
    }
    return result.content;
}

void OpenCodeServer::call_llm_stream(const ChatRequest& req, SseFrameQueue& frames, const json& tools) {
    auto prov = resolve_provider(req);
    prov->stream_chat(req, [&frames](std::string_view delta) {
        frames.push(acp::assistant_frame(delta));
    });
}

} // namespace opencode
