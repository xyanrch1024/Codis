#include "server.h"

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
    // 加载配置
    if (config_path && !config_path->empty()) {
        config_ = AppConfig::load(*config_path);
    }

    // 从配置注册 providers
    for (auto& pc : config_.providers) {
        pc.resolve_api_key();
        if (!pc.api_key.empty()) {
            provider_registry_.register_provider(pc);
        }
    }
    if (!config_.default_provider.empty()) {
        provider_registry_.set_default(config_.default_provider);
    }

    // 如果配置里没有，尝试环境变量注册默认 provider
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

    register_routes();
}

OpenCodeServer::~OpenCodeServer() { stop(); }

void OpenCodeServer::start() {
    running_ = true;
    thread_ = std::make_unique<std::thread>([this] {
        LOG_INFO("Server listening on http://localhost:{}", port_);
        auto providers = provider_registry_.list();
        for (auto& p : providers) LOG_INFO("  provider: {}", p);
        LOG_INFO("  default: {}", provider_registry_.default_name());
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
    acp::json j;
    j["status"] = "ok";
    j["version"] = "0.3.1";
    j["protocol"] = "acp";
    j["port"] = port_;
    j["default_provider"] = provider_registry_.default_name();
    res.set_content(j.dump(2), "application/json");
}

void OpenCodeServer::handle_info(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    acp::json j;
    j["providers"] = provider_registry_.list();
    j["default_provider"] = provider_registry_.default_name();
    j["features"] = {"acp", "chat", "stream", "sessions"};
    res.set_content(j.dump(2), "application/json");
}

void OpenCodeServer::handle_chat(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto body = acp::json::parse(req.body);
        auto chat_req = ChatRequest::from_json(body);
        std::string result = call_llm(chat_req);

        acp::json resp;
        resp["content"] = result;
        resp["model"] = chat_req.model;
        resp["success"] = true;
        res.set_content(resp.dump(), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(acp::json{{"error", e.what()}}.dump(), "application/json");
    }
}

void OpenCodeServer::handle_acp(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto body = acp::json::parse(req.body);
        auto chat_req = ChatRequest::from_json(body);
        chat_req.stream = true;

        auto frames = std::make_shared<SseFrameQueue>();

        std::thread llm_thread([this, req = std::move(chat_req), frames]() mutable {
            try {
                call_llm_stream(req, *frames);
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
        res.set_content(acp::json{{"error", e.what()}}.dump(), "application/json");
    }
}

void OpenCodeServer::handle_session_create(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    auto id = session_mgr_.create_session();
    res.status = 201;
    res.set_content(acp::json{{"session_id", id}}.dump(), "application/json");
}
void OpenCodeServer::handle_session_get(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    auto session = session_mgr_.get_session(req.matches[1]);
    if (!session) { res.status = 404; res.set_content(R"({"error":"not found"})", "application/json"); return; }
    acp::json j;
    j["id"] = session->id; j["model"] = session->model; j["provider"] = session->provider;
    j["messages"] = acp::json::array();
    for (auto& m : session->messages) j["messages"].push_back(m.to_json());
    res.set_content(j.dump(2), "application/json");
}
void OpenCodeServer::handle_session_add_message(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto msg = Message::from_json(acp::json::parse(req.body));
        session_mgr_.add_message(req.matches[1], msg);
        res.set_content(R"({"status":"ok"})", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(acp::json{{"error", e.what()}}.dump(), "application/json");
    }
}

// =============================================================================
// Provider 解析 + LLM 调用
// =============================================================================

std::shared_ptr<LLMProvider> OpenCodeServer::resolve_provider(const ChatRequest& req) {
    std::string name = req.provider.empty() ? provider_registry_.default_name() : req.provider;
    LOG_DEBUG("resolving provider '{}' (requested: '{}', default: '{}')",
              name, req.provider, provider_registry_.default_name());

    auto provider = provider_registry_.get(name);
    if (provider) return *provider;

    LOG_WARN("provider '{}' not found, falling back to first available", name);
    auto list = provider_registry_.list();
    if (!list.empty()) return *provider_registry_.get(list[0]);

    throw std::runtime_error("No provider configured");
}

std::string OpenCodeServer::call_llm(const ChatRequest& req) {
    auto prov = resolve_provider(req);
    auto result = prov->chat(req);
    if (!result.success) {
        LOG_ERROR("LLM call failed: {}", result.error);
        throw std::runtime_error(result.error);
    }
    return result.content;
}

void OpenCodeServer::call_llm_stream(const ChatRequest& req, SseFrameQueue& frames) {
    auto prov = resolve_provider(req);
    LOG_TRACE("starting LLM stream on provider '{}'", prov->name());
    prov->stream_chat(req, [&frames](std::string_view delta) {
        frames.push(acp::assistant_frame(delta));
    });
}

} // namespace opencode
