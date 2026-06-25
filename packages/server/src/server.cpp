#include "server.h"
#include "tools/tools.h"

#include <iostream>
#include <thread>
#include <random>
#include <sstream>
#include <iomanip>

namespace opencode {

namespace {
std::string expand_path(const std::string& path) {
    if (path.starts_with("~/")) {
        const char* home = std::getenv("HOME");
        return (home ? home : "/tmp") + path.substr(1);
    }
    return path;
}
} // anonymous namespace

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
void SessionManager::add_message(const std::string& id, const Message&) {
    // Legacy — 由 SessionStore 替代
    (void)id;
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

    init_context_sources();
    register_routes();
}

// =============================================================================
// Context Sources 初始化
// =============================================================================

void OpenCodeServer::init_context_sources() {
    system_context_.register_source(context_sources::date_source());
    system_context_.register_source(context_sources::working_dir_source());
    system_context_.register_source(context_sources::platform_source());
    system_context_.register_source(context_sources::git_status_source());
    system_context_.register_source(context_sources::project_instructions_source("."));
    system_context_.register_source(context_sources::tools_source([this]() {
        return tool_registry_.all_schemas();
    }));
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
    server_->Get(R"(/api/v1/acp/stream/([a-f0-9\-]+))", [this](auto& r, auto& s) { handle_acp_stream(r, s); });
    server_->Post("/api/v1/sessions",    [this](auto& r, auto& s) { handle_session_create(r, s); });
    server_->Get("/api/v1/sessions",     [this](auto& r, auto& s) { handle_session_list(r, s); });
    server_->Get(R"(/api/v1/sessions/([a-f0-9\-]+))",     [this](auto& r, auto& s) { handle_session_get(r, s); });
    server_->Delete(R"(/api/v1/sessions/([a-f0-9\-]+))",  [this](auto& r, auto& s) { handle_session_delete(r, s); });
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
        chat_req.tools = tools;

        std::string result = call_llm(chat_req);

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
        std::string sid = body.value("session_id", "");

        if (sid.empty() || !session_store_.load_session(sid))
            sid = session_store_.create_session();

        bool has_msg = false;
        for (auto& m : chat_req.messages)
            if (m.role == "user" && !m.content.empty()) has_msg = true;

        if (has_msg) {
            for (auto it = chat_req.messages.rbegin(); it != chat_req.messages.rend(); ++it) {
                if (it->role == "user" && !it->content.empty()) {
                    session_store_.append_message(sid, *it); break;
                }
            }

            // 检查是否有 LLM 正在运行
            {
                std::unique_lock lock(active_mutex_);
                auto& active = active_sessions_[sid];
                if (active.processing.exchange(true)) {
                    // 已有 LLM 在运行，消息会在当前轮结束后处理
                    lock.unlock();
                } else {
                    lock.unlock();
                    std::thread([this, sid, req = std::move(chat_req)]() mutable {
                        run_acp_loop_broadcast(sid, std::move(req));
                    }).detach();
                }
            }
        }

        res.status = 202;
        res.set_content(json{{"session_id", sid, "accepted", has_msg}}.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void OpenCodeServer::handle_acp_stream(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    std::string sid = req.matches[1];
    if (!session_store_.load_session(sid))
        session_store_.create_session_with_id(sid);

    auto queue = std::make_shared<SseFrameQueue>();
    std::string topic = "session:" + sid + ":event";

    // 订阅 EventBus: 所有帧推入此 client 的 queue
    auto sub_id = event_bus_.subscribe(topic, [queue](const std::string& frame) {
        queue->push(frame);
    });

    // 推历史消息
    auto history = session_store_.load_messages(sid);
    for (auto& m : history) {
        if (m.role == "user")
            queue->push(acp::assistant_frame("\n[User] " + m.content));
        else if (m.role == "assistant" && !m.content.empty())
            queue->push(acp::assistant_frame(m.content));
    }

    LOG_INFO("SSE stream attached to session {} (subscribers: {})",
             sid.substr(0, 8), event_bus_.subscriber_count(topic));

    // SSE 长连接
    res.set_chunked_content_provider("text/event-stream",
        [queue, this, topic, sub_id](size_t, httplib::DataSink& sink) -> bool {
            auto frame = queue->pop();
            if (frame.empty()) {
                event_bus_.unsubscribe(topic, sub_id);
                sink.done(); return false;
            }
            if (!sink.write(frame.data(), frame.size())) {
                event_bus_.unsubscribe(topic, sub_id);
                return false;
            }
            return true;
        });
}

// =============================================================================
// run_acp_loop_broadcast — EventBus 发布
// =============================================================================

void OpenCodeServer::run_acp_loop_broadcast(const std::string& session_id, ChatRequest req) {
    static const int MAX_TURNS = 10;
    std::string topic = "session:" + session_id + ":event";

    LOG_DEBUG("ACP loop started, session {}", session_id.substr(0, 8));

    json tools = json::array();
    for (auto& s : tool_registry_.all_schemas()) {
        tools.push_back({{"type", "function"}, {"function", {
            {"name", s.name}, {"description", s.description},
            {"parameters", s.parameters}
        }}});
    }
    req.tools = tools;

    auto baseline = system_context_.build_baseline(session_id, session_store_);
    req.messages.insert(req.messages.begin(), {"system", baseline});

    std::string assistant_content;
    auto turn = std::make_shared<int>(0);
    bool is_first_turn = true;

    while (*turn < MAX_TURNS) {
        (*turn)++;
        LOG_DEBUG("ACP loop turn {}/{}", *turn, MAX_TURNS);

        if (!is_first_turn) {
            auto update = system_context_.reconcile(session_id, session_store_);
            if (update) req.messages.push_back({"system", *update});
        }
        is_first_turn = false;

        assistant_content.clear();
        auto prov = resolve_provider(req);
        if (!prov) { event_bus_.publish(topic, acp::error_frame("No provider")); break; }
        prov->stream_chat(req, [&](std::string_view delta) {
            assistant_content += delta;
            event_bus_.publish(topic, acp::assistant_frame(delta));
        });

        if (!assistant_content.empty())
            session_store_.append_message(session_id, {"assistant", assistant_content});

        auto call_list = extract_tool_calls(assistant_content);
        if (call_list.empty()) break;

        for (auto& call : call_list) {
            auto perm = tool_registry_.check_permission(call.name);
            if (perm == Permission::Denied) {
                event_bus_.publish(topic, acp::tool_result_frame(call.id, false, "Permission denied"));
                continue;
            }
            auto result = tool_registry_.execute(call);
            event_bus_.publish(topic, acp::tool_result_frame(result.id, result.success, result.content));

            Message asst_msg; asst_msg.role = "assistant";
            asst_msg.tool_call_id = call.id; asst_msg.tool_name = call.name;
            req.messages.push_back(asst_msg);

            Message tool_msg; tool_msg.role = "tool";
            tool_msg.content = result.content; tool_msg.tool_call_id = call.id;
            req.messages.push_back(tool_msg);
        }
    }

    if (*turn >= MAX_TURNS)
        event_bus_.publish(topic, acp::error_frame("Max turns reached"));

    event_bus_.publish(topic, acp::done_frame());

    // 标记完成
    {
        std::unique_lock lock(active_mutex_);
        auto it = active_sessions_.find(session_id);
        if (it != active_sessions_.end()) it->second.processing = false;
    }
    LOG_DEBUG("session {} completed", session_id.substr(0, 8));
}

// =============================================================================
// Tool call 提取 — 从 token 流中解析
// =============================================================================

std::vector<ToolCall> OpenCodeServer::extract_tool_calls(const std::string& content) {
    std::vector<ToolCall> calls;
    auto pos = content.find("\"tool_calls\"");
    if (pos == std::string::npos) return calls;

    std::string json_str = content;
    // 尝试从 markdown 代码块中提取
    auto md_start = content.find("```json");
    if (md_start != std::string::npos) {
        md_start += 7; // skip "```json"
        auto md_end = content.find("```", md_start);
        if (md_end != std::string::npos)
            json_str = content.substr(md_start, md_end - md_start);
    }

    try {
        auto j = json::parse(json_str);
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
    } catch (...) {}
    return calls;
}

// =============================================================================
// 会话端点
// =============================================================================

void OpenCodeServer::handle_session_create(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    auto id = session_store_.create_session();
    res.status = 201;
    res.set_content(json{{"session_id", id}}.dump(), "application/json");
}

void OpenCodeServer::handle_session_list(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    auto sessions = session_store_.list_sessions_info();
    json arr = json::array();
    for (auto& s : sessions) {
        arr.push_back({
            {"id", s.id},
            {"title", s.title},
            {"message_count", s.message_count},
            {"created_at", s.created_at},
            {"updated_at", s.updated_at}
        });
    }
    res.set_content(arr.dump(), "application/json");
}

void OpenCodeServer::handle_session_get(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    auto session = session_store_.load_session(req.matches[1]);
    if (!session) { res.status = 404; res.set_content(R"({"error":"not found"})", "application/json"); return; }
    json j;
    j["id"] = session->id;
    auto msgs = session_store_.load_messages(session->id);
    j["messages"] = json::array();
    for (auto& m : msgs) j["messages"].push_back(m.to_json());
    res.set_content(j.dump(2), "application/json");
}

void OpenCodeServer::handle_session_delete(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    session_store_.delete_session(req.matches[1]);
    res.set_content(R"({"status":"ok"})", "application/json");
}

void OpenCodeServer::handle_session_add_message(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto msg = Message::from_json(json::parse(req.body));
        session_store_.append_message(req.matches[1], msg);
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

    LOG_ERROR("no provider configured");
    return nullptr;
}
std::string OpenCodeServer::call_llm(const ChatRequest& req) {
    auto prov = resolve_provider(req);
    if (!prov) throw std::runtime_error("No provider configured. Set API key env var (e.g. GLM_API_KEY)");
    auto result = prov->chat(req);
    if (!result.success) {
        LOG_ERROR("LLM call failed: {}", result.error);
        throw std::runtime_error(result.error);
    }
    return result.content;
}

void OpenCodeServer::call_llm_stream(const ChatRequest& req, SseFrameQueue& frames) {
    auto prov = resolve_provider(req);
    if (!prov) throw std::runtime_error("No provider configured. Set API key env var (e.g. GLM_API_KEY)");
    prov->stream_chat(req, [&frames](std::string_view delta) {
        frames.push(acp::assistant_frame(delta));
    });
}

} // namespace opencode
