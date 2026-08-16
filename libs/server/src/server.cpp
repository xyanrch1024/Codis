#include "server.h"
#include "tools/tools.h"
#include "tools/skill_tool.h"
#include "plugin_loader.h"
#include "plugin_tool.h"
#include "str_util.h"

#include <iostream>
#include <thread>
#include <random>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <sys/socket.h>

namespace codis {

// 定义在 Tool call 提取段（extract_tool_calls 前），供存历史时剥离内嵌 JSON 使用
static std::pair<size_t, size_t> tool_calls_json_span(const std::string& content);

// 纯空白判断（剥掉 tool_calls JSON 后模型输出可能只剩换行）
static bool is_blank(const std::string& s) {
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    return true;
}

// =============================================================================
// FrameQueue
// =============================================================================

void FrameQueue::push(std::string frame) {
    { std::lock_guard lock(mutex_); queue_.push(std::move(frame)); }
    cv_.notify_one();
}
std::string FrameQueue::pop() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return "";
    auto frame = std::move(queue_.front()); queue_.pop();
    return frame;
}
void FrameQueue::close() { closed_ = true; cv_.notify_all(); }

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
// CodisServer
// =============================================================================

CodisServer::CodisServer(int port, std::optional<std::string> config_path)
    : port_(port)
    , server_(std::make_unique<httplib::Server>())
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
    tool_registry_.register_tool(std::make_unique<tools::WebSearchTool>(tools::WebSearchOptions{
        config_.websearch.backend, config_.websearch.api_key,
        config_.websearch.max_results, config_.websearch.timeout_seconds}));
    tool_registry_.register_tool(std::make_unique<tools::SkillTool>(config_.skills.dirs));

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

    init_context_sources();
    register_routes();
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
    LOG_INFO("Server stopped");
}

void CodisServer::set_cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

std::string CodisServer::generate_conn_id() {
    return util::gen_short_id();
}

void CodisServer::cleanup_connection(const std::string& sid, const std::string& conn_id) {
    LOG_INFO("WS connection detached session {} conn_id={}", sid.substr(0, 8), conn_id);
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(sid);
    if (it == sessions_.end()) return;
    it->second.conns.erase(conn_id);
    if (it->second.conns.empty())
        sessions_.erase(it);
}

// =============================================================================
// 路由
// =============================================================================

void CodisServer::register_routes() {
    server_->Options("/api/v1/.*", [](const auto&, auto& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        res.status = 204;
    });
    server_->Get("/api/v1/health",       [this](auto& r, auto& s) { handle_health(r, s); });
    server_->Get("/api/v1/info",         [this](auto& r, auto& s) { handle_info(r, s); });
    server_->Post("/api/v1/chat",        [this](auto& r, auto& s) { handle_chat(r, s); });
    server_->Post("/api/v1/acp/switch",  [this](auto& r, auto& s) { handle_acp_switch(r, s); });
    server_->WebSocket(R"(/api/v1/acp/ws/([a-f0-9\-]+))", [this](auto& r, auto& ws) { handle_acp_ws(r, ws); });
    server_->Post("/api/v1/sessions",    [this](auto& r, auto& s) { handle_session_create(r, s); });
    server_->Get("/api/v1/sessions",     [this](auto& r, auto& s) { handle_session_list(r, s); });
    server_->Get(R"(/api/v1/sessions/([a-f0-9\-]+))",     [this](auto& r, auto& s) { handle_session_get(r, s); });
    server_->Delete("/api/v1/sessions",            [this](auto& r, auto& s) { handle_session_delete_all(r, s); });
    server_->Delete(R"(/api/v1/sessions/([a-f0-9\-]+))", [this](auto& r, auto& s) { handle_session_delete(r, s); });
    server_->Post(R"(/api/v1/sessions/([a-f0-9\-]+)/messages)", [this](auto& r, auto& s) { handle_session_add_message(r, s); });
    server_->Get(R"(/api/v1/balance/([a-zA-Z0-9_\-]+))", [this](auto& r, auto& s) { handle_balance(r, s); });
}

// =============================================================================
// 端点
// =============================================================================

void CodisServer::handle_health(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    json j;
    j["status"] = "ok";
    j["version"] = "0.4.0";
    j["protocol"] = "acp";
    j["port"] = port_;
    j["default_provider"] = provider_registry_.default_name();
    j["tools"] = tool_registry_.list();
    res.set_content(json_dump_safe(j, 2), "application/json");
}

void CodisServer::handle_info(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    json j;
    j["providers"] = provider_registry_.list();
    j["default_provider"] = provider_registry_.default_name();
    json pm = json::object();
    for (auto& p : config_.providers) pm[p.name] = p.model;
    j["provider_models"] = pm;
    j["tools"] = tool_registry_.list();
    j["features"] = {"acp", "chat", "websocket", "tools", "sessions"};
    res.set_content(json_dump_safe(j, 2), "application/json");
}

void CodisServer::handle_chat(const httplib::Request& req, httplib::Response& res) {
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

        // 与 ACP 循环对称：session 存在时忽略请求自带 messages，
        // 上下文改为 baseline + SQLite 历史 + 请求中最后一条 user 消息，
        // 客户端无需再传输整段历史。
        if (!chat_req.session_id.empty() && session_store_.load_session(chat_req.session_id)) {
            auto baseline = system_context_.build_baseline(chat_req.session_id, session_store_);
            std::vector<Message> msgs;
            msgs.push_back({"system", baseline});
            auto history = session_store_.load_messages(chat_req.session_id);
            for (auto& m : history)
                if (m.role == "user" || m.role == "tool" ||
                    (m.role == "assistant" && (m.tool_call_id || !is_blank(m.content))))
                    msgs.push_back(m);
            for (auto it = chat_req.messages.rbegin(); it != chat_req.messages.rend(); ++it)
                if (it->role == "user" && !it->content.empty()) {
                    msgs.push_back(*it);
                    break;
                }
            chat_req.messages = std::move(msgs);
        }

        std::string result = call_llm(chat_req);

        json resp;
        resp["content"] = result;
        resp["model"] = chat_req.model;
        resp["success"] = true;
        res.set_content(json_dump_safe(resp), "application/json");
    } catch (const std::exception& e) {
        res.status = 500;
        res.set_content(json_dump_safe(json{{"error", e.what()}}), "application/json");
    }
}

void CodisServer::queue_chat_request(const std::string& session_id,
                                        const std::string& conn_id, ChatRequest req) {
    if (session_id.empty() || !session_store_.load_session(session_id))
        throw std::runtime_error("session not found: " + session_id);

    bool has_msg = false;
    for (auto& m : req.messages)
        if (m.role == "user" && !m.content.empty()) has_msg = true;

    if (has_msg) {
        for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
            if (it->role == "user" && !it->content.empty()) {
                // 首条消息时用其内容生成会话标题
                if (session_store_.message_count(session_id) == 0) {
                    auto title = util::make_session_title(it->content);
                    if (!title.empty()) session_store_.set_title(session_id, title);
                }
                session_store_.append_message(session_id, *it); break;
            }
        }

        // 检查是否有 LLM 正在运行（按 session）
        // 运行中则排队，当前轮结束后自动补跑，避免消息被静默丢弃
        bool should_run = false;
        {
            std::lock_guard lock(sessions_mutex_);
            auto& state = sessions_[session_id];
            if (!state.processing.exchange(true))
                should_run = true;
            else
                state.pending.push_back(req);
        }
        if (should_run) {
            std::thread([this, session_id, conn_id, req = std::move(req)]() mutable {
                run_acp_loop_broadcast(session_id, conn_id, std::move(req));
            }).detach();
        }
    }
}

void CodisServer::handle_acp_ws(const httplib::Request& req, httplib::ws::WebSocket& ws) {
    std::string sid = req.matches[1];
    if (!session_store_.load_session(sid))
        session_store_.create_session_with_id(sid);

    auto queue = std::make_shared<FrameQueue>();
    std::string conn_id = generate_conn_id();

    {
        std::lock_guard lock(sessions_mutex_);
        sessions_[sid].conns[conn_id] = queue;
    }

    // 首帧：告知客户端其 conn_id
    queue->push(acp::connected_frame(conn_id));

    LOG_INFO("WS connection attached to session {} conn_id={}",
             sid.substr(0, 8), conn_id);

    // 发送线程：从 queue 取帧 → ws.send；队列关闭（pop 返回空）→ 关闭连接
    std::thread sender([queue, &ws]() {
        while (true) {
            auto frame = queue->pop();
            if (frame.empty()) { ws.close(); break; }
            ws.send(frame);
        }
    });

    // 读循环（全双工）：接收客户端 request 帧 + 检测断开（read 返回 Fail）
    std::string msg;
    while (ws.read(msg) != httplib::ws::ReadResult::Fail) {
        auto event = acp::parse_frame(msg);
        if (!event) {
            LOG_WARN("WS request frame parse failed: {}", msg);
            ws.send(acp::error_frame("invalid frame"));
            continue;
        }
        if (event->type != acp::EventType::request &&
            event->type != acp::EventType::switch_session &&
            event->type != acp::EventType::cancel &&
            event->type != acp::EventType::confirm_ack) {
            LOG_WARN("WS unexpected frame type: {}", acp::to_string(event->type));
            ws.send(acp::error_frame("unsupported frame type"));
            continue;
        }
        try {
            if (event->type == acp::EventType::confirm_ack) {
                // 工具确认回执：唤醒等待该 confirm_id 的挂起确认
                std::string confirm_id = event->data.value("confirm_id", "");
                bool approved = event->data.value("approved", false);
                std::shared_ptr<PendingConfirm> slot;
                {
                    std::lock_guard lock(sessions_mutex_);
                    for (auto& [sid, st] : sessions_) {
                        auto it = st.pending_confirms.find(confirm_id);
                        if (it != st.pending_confirms.end()) {
                            slot = it->second;
                            break;
                        }
                    }
                }
                if (slot) {
                    {
                        std::lock_guard lk(slot->mutex);
                        slot->approved = approved;
                        slot->answered = true;
                    }
                    slot->cv.notify_one();
                    LOG_INFO("confirm_ack {} approved={}", confirm_id, approved);
                } else {
                    LOG_WARN("confirm_ack for unknown confirm_id: {}", confirm_id);
                }
                continue;
            }
            if (event->type == acp::EventType::switch_session) {
                std::string target_sid = event->data.value("session_id", "");
                if (target_sid.empty()) {
                    ws.send(acp::error_frame("switch requires session_id"));
                    continue;
                }
                if (!session_store_.load_session(target_sid))
                    session_store_.create_session_with_id(target_sid);
                if (!move_connection(conn_id, target_sid)) {
                    LOG_ERROR("WS switch failed: conn {} not found", conn_id);
                    ws.send(acp::error_frame("conn not found"));
                } else {
                    LOG_INFO("WS switch conn={} -> session={}",
                             conn_id.substr(0, 8), target_sid.substr(0, 8));
                }
                continue;
            }
            if (event->type == acp::EventType::cancel) {
                // 取消当前 session 正在执行的任务（LLM 流 + 工具循环）
                std::string target_sid = event->data.value("session_id", "");
                if (target_sid.empty()) target_sid = sid;
                {
                    std::lock_guard lock(sessions_mutex_);
                    auto it = sessions_.find(target_sid);
                    if (it != sessions_.end()) {
                        it->second.cancel_requested->store(true);
                        it->second.pending.clear();  // 取消后排队消息一并清空
                        // 挂起的权限确认立即终止（视为拒绝），唤醒等待线程
                        for (auto& [_, slot] : it->second.pending_confirms) {
                            std::lock_guard lk(slot->mutex);
                            slot->approved = false;
                            slot->answered = true;
                        }
                        for (auto& [_, slot] : it->second.pending_confirms)
                            slot->cv.notify_all();
                        LOG_INFO("session {} cancel requested by conn {}",
                                 target_sid.substr(0, 8), conn_id.substr(0, 8));
                    }
                }
                continue;
            }
            auto chat_req = ChatRequest::from_json(event->data);
            // 客户端可能已通过 switch 切到其它 session，优先用帧内 session_id
            std::string target_sid = event->data.value("session_id", "");
            if (target_sid.empty()) target_sid = sid;
            queue_chat_request(target_sid, conn_id, std::move(chat_req));
        } catch (const std::exception& e) {
            LOG_ERROR("WS request processing failed: {}", e.what());
            ws.send(acp::error_frame(e.what()));
        }
    }

    queue->close();
    if (sender.joinable()) sender.join();
    cleanup_connection(sid, conn_id);
}

// =============================================================================
// handle_acp_switch — 切换 session 不断开 WS
// =============================================================================

bool CodisServer::move_connection(const std::string& conn_id, const std::string& new_sid) {
    std::shared_ptr<FrameQueue> queue;

    {
        std::lock_guard lock(sessions_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            auto qit = it->second.conns.find(conn_id);
            if (qit != it->second.conns.end()) {
                queue = qit->second;
                it->second.conns.erase(qit);
                if (it->second.conns.empty())
                    sessions_.erase(it);
                break;
            }
        }
        if (queue)
            sessions_[new_sid].conns[conn_id] = queue;
    }

    if (!queue) return false;
    queue->push(acp::connected_frame(conn_id));
    return true;
}

void CodisServer::handle_acp_switch(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto body = json::parse(req.body);
        auto conn_id = body.value("conn_id", "");
        auto new_sid = body.value("session_id", "");

        if (conn_id.empty() || new_sid.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"conn_id and session_id required"})", "application/json");
            return;
        }

        if (!session_store_.load_session(new_sid))
            session_store_.create_session_with_id(new_sid);

        if (!move_connection(conn_id, new_sid)) {
            res.status = 400;
            res.set_content(R"({"error":"conn_id not found"})", "application/json");
            return;
        }

        res.set_content(R"({"status":"ok"})", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

// =============================================================================
// run_acp_loop_broadcast — 推送到指定 connection 的 queue
// =============================================================================

void CodisServer::run_acp_task(const std::string& session_id,
                               const std::string& conn_id, ChatRequest req) {
    static const int MAX_TURNS = 100;
    static const int MAX_EMPTY_RETRIES = 2;
    static const int MAX_MALFORMED_RETRIES = 3;
    int empty_retries_ = 0;
    int malformed_retries_ = 0;

    auto broadcast = [&](const std::string& frame) {
        std::lock_guard lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return;
        if (conn_id.empty()) {
            // conn_id 为空时发送到所有连接
            for (auto& [_, q] : it->second.conns)
                q->push(frame);
        } else {
            auto qit = it->second.conns.find(conn_id);
            if (qit != it->second.conns.end()) {
                qit->second->push(frame);
            } else {
                // 帧发不出去：目标 conn 不存在（已断连/换了端口/请求被其它实例接收）
                // 打日志以便诊断"服务端有消息但客户端收不到"
                LOG_WARN("broadcast drop: conn {} not in session {} ({} conns attached)",
                         conn_id, session_id.substr(0, 8), it->second.conns.size());
            }
        }
    };

    LOG_DEBUG("ACP loop started, session {} conn={}", session_id.substr(0, 8), conn_id);

    json tools = json::array();
    for (auto& s : tool_registry_.all_schemas()) {
        tools.push_back({{"type", "function"}, {"function", {
            {"name", s.name}, {"description", s.description},
            {"parameters", s.parameters}
        }}});
    }
    req.tools = tools;

    auto baseline = system_context_.build_baseline(session_id, session_store_);
    // store 历史已含 queue_chat_request 刚 append 的当前 user 消息，整体重放：
    // 顺序 = system baseline + 历史正序。重放只取 user 与无工具引用的 assistant
    // 纯文本——带 tool_call_id 的中转消息（无正文）直接跳过：
    // 同轮工具往返已在本轮 req.messages 中，跨轮重放悬浮 tool_call 会导致
    // OpenAI 格式断链（严格 provider 报错/模型困惑），且白白多占 token。
    auto history = session_store_.load_messages(session_id);
    std::vector<Message> msgs;
    msgs.push_back({"system", baseline});
    for (auto& m : history) {
        // 重放：user 消息 + 纯文本 assistant + 完整工具往返
        // （assistant 中转带 tool_call_id；tool 结果成对紧随其后，序列化
        // 已输出标准 tool_calls 数组，模型能识别历史调用，避免重复执行）
        if (m.role == "user" || m.role == "tool" ||
            (m.role == "assistant" && (m.tool_call_id || !is_blank(m.content))))
            msgs.push_back(m);
    }
    req.messages = std::move(msgs);

    std::string assistant_content;
    auto turn = std::make_shared<int>(0);
    bool is_first_turn = true;

    // 取消标志：从 session 状态取共享指针，保证跨线程安全
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    {
        std::lock_guard lock(sessions_mutex_);
        cancel_flag = sessions_[session_id].cancel_requested;
    }
    cancel_flag->store(false);  // 本轮任务开始时清掉旧的取消标记

    auto is_canceled = [&] {
        return cancel_flag->load();
    };

    while (*turn < MAX_TURNS) {
        (*turn)++;
        LOG_DEBUG("ACP loop turn {}/{}", *turn, MAX_TURNS);

        if (is_canceled()) {
            broadcast(acp::error_frame("canceled"));
            break;
        }

        if (!is_first_turn) {
            auto update = system_context_.reconcile(session_id, session_store_);
            if (update) req.messages.push_back({"system", *update});
        }
        is_first_turn = false;

        assistant_content.clear();
        auto prov = resolve_provider(req);
        if (!prov) { broadcast(acp::error_frame("No provider")); break; }

        auto t0 = std::chrono::steady_clock::now();
        auto llm_result = prov->stream_chat(
            req,
            [&](std::string_view delta) {
                assistant_content += delta;
                // tool_calls JSON 不作为文本广播，客户端改走 tool_call 帧
                if (delta.find("\"tool_calls\"") != std::string_view::npos) return;
                broadcast(acp::assistant_frame(delta));
            },
            [&](std::string_view delta) {
                // 空 reasoning delta（混合思考模型的非思考请求会发 ""）：
                // 直接透传会让客户端渲染出只有标签的空思维链块
                if (delta.empty()) {
                    LOG_TRACE("session {} reasoning empty delta skipped", session_id.substr(0, 8));
                    return;
                }
                broadcast(acp::reasoning_frame(delta));
            },
            cancel_flag.get());

        auto llm_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        // 客户端取消：LLM 流被中断，结束本轮，不执行工具
        if (llm_result.canceled || cancel_flag->load()) {
            LOG_INFO("session {} task canceled after {}ms", session_id.substr(0, 8), llm_ms);
            broadcast(acp::done_frame());
            break;
        }

        if (!llm_result.success) {
            LOG_ERROR("LLM call failed after {}ms: {}", llm_ms, llm_result.error);
            broadcast(acp::error_frame("LLM 调用失败: " + llm_result.error));
            break;
        }

        if (!llm_result.reasoning_content.empty())
            LOG_DEBUG("turn {} reasoning_content ({} bytes):\n{}", *turn,
                      llm_result.reasoning_content.size(), llm_result.reasoning_content);

        LOG_DEBUG("turn {} LLM: {}ms {} tokens: {}", *turn, llm_ms,
                  assistant_content.size(), assistant_content.substr(0, 200));
        LOG_DEBUG("turn {} LLM full output ({} bytes):\n{}", *turn,
                  assistant_content.size(), assistant_content);

        // 持久化思维链（独立行，恢复会话时按顺序在 assistant 前展示；不进入模型上下文）
        if (!llm_result.reasoning_content.empty())
            session_store_.append_message(session_id, {"reasoning", llm_result.reasoning_content});

        // 存历史时剥掉内嵌的 tool_calls JSON（只存可见文本），避免恢复会话时
        // 把原始 JSON 当正文展示给用户/模型
        std::string assistant_text = assistant_content;
        {
            auto [jbegin, jend] = tool_calls_json_span(assistant_content);
            if (jbegin != std::string::npos && jbegin < jend)
                assistant_text = assistant_content.substr(0, jbegin) + assistant_content.substr(jend);
        }
        // 剥完 JSON 后若只剩空白（模型输出只用 tool_calls JSON 时残留换行），
        // 不入库，避免空白 assistant 条目污染展示与重放上下文
        if (!assistant_text.empty() && !is_blank(assistant_text))
            session_store_.append_message(session_id, {"assistant", assistant_text});

        auto call_list = extract_tool_calls(assistant_content);
        if (call_list.empty()) {
            // 空响应保护：模型只回思维链或直接停（GLM thinking 耗尽 max_tokens 时 content 为空）
            // 重试一次，仍空则明确报错，避免"无输出就完成"
            if (assistant_content.empty()) {
                if (empty_retries_ < MAX_EMPTY_RETRIES) {
                    empty_retries_++;
                    LOG_WARN("LLM returned empty response (reasoning {} bytes), retry {}/{}",
                             llm_result.reasoning_content.size(), empty_retries_, MAX_EMPTY_RETRIES);
                    continue;
                }
                broadcast(acp::error_frame(
                    "模型返回空响应（可能思维链耗尽 max_tokens）。reasoning_content 大小: " +
                    std::to_string(llm_result.reasoning_content.size()) + " bytes"));
            } else if (assistant_content.find("\"tool_calls\"") != std::string::npos) {
                // 模型尝试调用工具但 JSON 解析失败：提示重试，而不是静默结束任务
                if (malformed_retries_ < MAX_MALFORMED_RETRIES) {
                    malformed_retries_++;
                    LOG_WARN("turn {} malformed tool_calls JSON, asking model to retry ({}/{})",
                             *turn, malformed_retries_, MAX_MALFORMED_RETRIES);
                    req.messages.push_back({"system",
                        "你上一条回复的 tool_calls JSON 格式错误（括号不匹配或含非法字符），无法执行。"
                        "请重新输出语法正确的 tool_calls JSON，不要附加多余文本。"});
                    continue;
                }
                broadcast(acp::error_frame("模型连续返回格式错误的 tool_calls JSON"));
            }
            break;
        }

        for (auto& call : call_list) {
            auto perm = tool_registry_.check_permission(call.name);
            if (perm == Permission::Denied) {
                broadcast(acp::tool_call_frame(call.id, call.name, call.arguments));
                broadcast(acp::tool_result_frame(call.id, false, "Permission denied"));
                continue;
            }
            if (perm == Permission::Ask) {
                // Ask 权限：先征询用户确认，批准后才广播 tool_call 帧并执行
                if (!wait_for_confirmation(session_id, call, broadcast, cancel_flag)) {
                    broadcast(acp::tool_result_frame(call.id, false,
                        is_canceled() ? "Canceled while awaiting confirmation"
                                      : "Tool call rejected (confirmation declined or timed out)"));
                    if (is_canceled()) break;  // 任务被取消，结束整个 ACP 循环
                    continue;
                }
            }
            broadcast(acp::tool_call_frame(call.id, call.name, call.arguments));
            auto result = tool_registry_.execute(call);
            broadcast(acp::tool_result_frame(result.id, result.success, result.content));

            Message asst_msg; asst_msg.role = "assistant";
            asst_msg.tool_call_id = call.id; asst_msg.tool_name = call.name;
            asst_msg.tool_arguments = call.arguments;
            req.messages.push_back(asst_msg);
            session_store_.append_message(session_id, asst_msg);

            Message tool_msg; tool_msg.role = "tool";
            tool_msg.content = result.content; tool_msg.tool_call_id = call.id;
            req.messages.push_back(tool_msg);
            session_store_.append_message(session_id, tool_msg);
        }
    }

    if (*turn >= MAX_TURNS)
        broadcast(acp::error_frame("Max turns reached"));

    broadcast(acp::done_frame());
}

// =============================================================================
// run_acp_loop_broadcast — 顶层异常防护 + 排队请求迭代补跑
// run_acp_task 抛出异常（数据库损坏、Provider 返回畸形 JSON 等）时不能让
// detached 线程终止进程；排队请求用循环补跑而不是递归，避免刷帧导致栈溢出。
// =============================================================================

void CodisServer::run_acp_loop_broadcast(const std::string& session_id,
                                         const std::string& conn_id, ChatRequest req) {
    auto broadcast = [&](const std::string& frame) {
        std::lock_guard lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) return;
        if (conn_id.empty()) {
            for (auto& [_, q] : it->second.conns) q->push(frame);
        } else {
            auto qit = it->second.conns.find(conn_id);
            if (qit != it->second.conns.end()) qit->second->push(frame);
        }
    };

    for (;;) {
        try {
            run_acp_task(session_id, conn_id, std::move(req));
        } catch (const std::exception& e) {
            LOG_ERROR("ACP task crashed, session {}: {}", session_id.substr(0, 8), e.what());
            broadcast(acp::error_frame(std::string("internal error: ") + e.what()));
            broadcast(acp::done_frame());
        } catch (...) {
            LOG_ERROR("ACP task crashed, session {}: unknown exception", session_id.substr(0, 8));
            broadcast(acp::error_frame("internal error: unknown exception"));
            broadcast(acp::done_frame());
        }
        // 有排队请求则保持 processing，在本线程内迭代补跑（不递归、不新建线程）
        std::optional<ChatRequest> next;
        {
            std::lock_guard lock(sessions_mutex_);
            auto it = sessions_.find(session_id);
            if (it != sessions_.end()) {
                if (!it->second.pending.empty()) {
                    next = std::move(it->second.pending.front());
                    it->second.pending.pop_front();
                } else {
                    it->second.processing = false;
                }
            }
        }
        if (!next) break;
        LOG_DEBUG("session {} rerun for queued message", session_id.substr(0, 8));
        req = std::move(*next);
    }
    LOG_DEBUG("session {} completed", session_id.substr(0, 8));
}

// =============================================================================
// wait_for_confirmation — Ask 权限工具的执行前确认
// 广播 tool_confirm 帧给 session 的所有连接，挂起等待任一连接的 confirm_ack。
// 超时（config [permissions].confirm_timeout）视为拒绝；任务被取消立即返回 false。
// =============================================================================

bool CodisServer::wait_for_confirmation(const std::string& session_id,
                                        const ToolCall& call,
                                        const std::function<void(const std::string&)>& broadcast,
                                        const std::shared_ptr<std::atomic<bool>>& cancel_flag) {
    auto slot = std::make_shared<PendingConfirm>();
    slot->call = call;
    std::string confirm_id = util::gen_short_id();

    {
        std::lock_guard lock(sessions_mutex_);
        auto& st = sessions_[session_id];
        st.pending_confirms[confirm_id] = slot;
    }

    LOG_INFO("session {} awaiting confirmation for tool '{}' ({})",
             session_id.substr(0, 8), call.name, confirm_id);
    broadcast(acp::tool_confirm_frame(confirm_id, call.id, call.name, call.arguments,
                                      config_.permissions.confirm_timeout_seconds));

    int timeout_s = config_.permissions.confirm_timeout_seconds;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    bool canceled = false;
    {
        std::unique_lock lk(slot->mutex);
        while (!slot->answered) {
            if (cancel_flag && cancel_flag->load()) { canceled = true; break; }
            if (std::chrono::steady_clock::now() >= deadline) break;
            // 分段等待：200ms 粒度轮询取消标记，避免取消后空等整个超时
            slot->cv.wait_until(
                lk, std::chrono::steady_clock::now() + std::chrono::milliseconds(200));
        }
    }

    bool approved = slot->answered && slot->approved;
    {
        std::lock_guard lock(sessions_mutex_);
        auto it = sessions_.find(session_id);
        if (it != sessions_.end()) it->second.pending_confirms.erase(confirm_id);
    }

    LOG_INFO("session {} confirmation for tool '{}': {}", session_id.substr(0, 8), call.name,
             canceled ? "canceled"
                      : (slot->answered ? (approved ? "approved" : "rejected") : "timed out"));
    return approved;
}

// =============================================================================
// Tool call 提取 — 从 token 流中解析
// =============================================================================

// 定位 content 中 tool_calls 最外层 JSON 的字节区间 [begin, end)（含 ```json 围栏）。
// 找不到返回 {npos, npos}；JSON 被截断时剥到末尾（保证文本部分可安全取出）。
static std::pair<size_t, size_t> tool_calls_json_span(const std::string& content) {
    auto pos = content.find("\"tool_calls\"");
    if (pos == std::string::npos) return {std::string::npos, std::string::npos};

    // 优先从 markdown 代码块中提取
    auto md_start = content.find("```json");
    if (md_start != std::string::npos && md_start < pos) {
        auto md_end = content.find("```", md_start + 7);
        if (md_end == std::string::npos) return {md_start, content.size()};
        return {md_start, md_end + 3};
    }

    // LLM 可能在 tool_calls JSON 前输出文本，找到包含 "tool_calls" 的最外层 JSON 对象
    // 用括号栈定位闭合位置（截断/漏写右括号时剥到末尾）
    auto brace = content.rfind('{', pos);
    if (brace == std::string::npos) return {std::string::npos, std::string::npos};

    std::string stack;
    bool in_string = false;
    bool escaped = false;
    for (auto i = brace; i < content.size(); i++) {
        char c = content[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') { in_string = !in_string; continue; }
        if (in_string) continue;
        if (c == '{') { stack += '{'; }
        else if (c == '[') { stack += '['; }
        else if (c == '}') {
            if (!stack.empty() && stack.back() == '{') stack.pop_back();
            if (stack.empty()) return {brace, i + 1};
        } else if (c == ']') {
            // 数组内还有未闭合对象：先补 } 再闭合 ]
            while (!stack.empty() && stack.back() == '{') stack.pop_back();
            if (!stack.empty() && stack.back() == '[') stack.pop_back();
            if (stack.empty()) return {brace, i + 1};
        }
    }
    return {brace, content.size()};
}

std::vector<ToolCall> CodisServer::extract_tool_calls(const std::string& content) {
    std::vector<ToolCall> calls;
    auto pos = content.find("\"tool_calls\"");
    if (pos == std::string::npos) return calls;

    std::string json_str;

    // 优先从 markdown 代码块中提取
    auto md_start = content.find("```json");
    if (md_start != std::string::npos) {
        md_start += 7;
        auto md_end = content.find("```", md_start);
        if (md_end != std::string::npos)
            json_str = content.substr(md_start, md_end - md_start);
    } else {
        // LLM 可能在 tool_calls JSON 前输出文本，找到包含 "tool_calls" 的最外层 JSON 对象
        // 用括号栈自动补齐模型偶尔截断/漏写的右括号（如缺 } 或提前 ]）
        auto brace = content.rfind('{', pos);
        if (brace == std::string::npos) return calls;
        std::string stack;
        std::string fixed;
        bool in_string = false;
        bool escaped = false;
        auto close_outer = [&] {
            if (stack.empty()) return;
            while (!fixed.empty() && fixed.back() == ',') fixed.pop_back();
            while (!stack.empty()) {
                fixed += stack.back() == '{' ? '}' : ']';
                stack.pop_back();
            }
        };
        for (auto i = brace; i < content.size(); i++) {
            char c = content[i];
            if (escaped) { escaped = false; fixed += c; continue; }
            if (c == '\\') { escaped = true; fixed += c; continue; }
            if (c == '"') { in_string = !in_string; fixed += c; continue; }
            if (in_string) { fixed += c; continue; }
            if (c == '{') { stack += '{'; fixed += c; }
            else if (c == '[') { stack += '['; fixed += c; }
            else if (c == '}') {
                while (!fixed.empty() && fixed.back() == ',') fixed.pop_back();
                if (!stack.empty() && stack.back() == '{') { stack.pop_back(); fixed += c; }
                if (stack.empty()) break;
            } else if (c == ']') {
                // 数组内还有未闭合对象：先补 } 再闭合 ]
                while (!fixed.empty() && fixed.back() == ',') fixed.pop_back();
                while (!stack.empty() && stack.back() == '{') { fixed += '}'; stack.pop_back(); }
                if (!stack.empty() && stack.back() == '[') { stack.pop_back(); fixed += c; }
                if (stack.empty()) break;
            } else if (c != ',') {
                fixed += c;
            } else if (!fixed.empty() && fixed.back() != ',') {
                fixed += c;
            }
        }
        close_outer();
        json_str = fixed;
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

void CodisServer::handle_session_create(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    auto id = session_store_.create_session();
    res.status = 201;
    res.set_content(json{{"session_id", id}}.dump(), "application/json");
}

void CodisServer::handle_session_list(const httplib::Request&, httplib::Response& res) {
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

void CodisServer::handle_session_get(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    auto session = session_store_.load_session(req.matches[1]);
    if (!session) { res.status = 404; res.set_content(R"({"error":"not found"})", "application/json"); return; }
    json j;
    j["id"] = session->id;
    auto msgs = session_store_.load_messages(session->id);
    j["messages"] = json::array();
    for (auto& m : msgs) j["messages"].push_back(m.to_json());
    res.set_content(json_dump_safe(j, 2), "application/json");
}

void CodisServer::handle_session_delete(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    session_store_.delete_session(req.matches[1]);
    res.set_content(R"({"status":"ok"})", "application/json");
}

void CodisServer::handle_session_delete_all(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    session_store_.delete_all_sessions();
    res.set_content(R"({"status":"ok","deleted":"all"})", "application/json");
}

void CodisServer::handle_session_add_message(const httplib::Request& req, httplib::Response& res) {
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
// 余额查询
// =============================================================================

void CodisServer::handle_balance(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    std::string provider_name = req.matches[1];

    try {
        auto result = query_provider_balance(provider_name);
        res.set_content(result.dump(2), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

json CodisServer::query_provider_balance(const std::string& provider_name) {
    // 查找 provider 配置
    auto providers = provider_registry_.list();
    const ProviderConfig* target = nullptr;

    for (auto& pc : config_.providers) {
        if (pc.name == provider_name) {
            target = &pc;
            break;
        }
    }

    if (!target) {
        throw std::runtime_error("Provider '" + provider_name + "' not found in config");
    }

    if (target->api_key.empty()) {
        throw std::runtime_error("No API key for provider '" + provider_name + "'");
    }

    // 解析 base_url
    std::string base = target->base_url;
    // 移除末尾的 /v1 等 path 部分，保留根 URL
    auto balance_url = base;
    // 对于 DeepSeek: https://api.deepseek.com -> https://api.deepseek.com/user/balance
    // 尝试去除末尾的 /v1 路径段
    if (balance_url.ends_with("/v1")) {
        balance_url = balance_url.substr(0, balance_url.size() - 3);
    } else if (balance_url.ends_with("/v1/")) {
        balance_url = balance_url.substr(0, balance_url.size() - 4);
    }
    // 确保末尾没有斜杠
    while (!balance_url.empty() && balance_url.back() == '/')
        balance_url.pop_back();

    std::string balance_endpoint = balance_url + "/user/balance";

    LOG_DEBUG("querying balance for '{}' at {}", provider_name, balance_endpoint);

    // 解析 host 和 path
    std::string url_part;
    bool use_ssl = false;

    if (balance_endpoint.starts_with("https://")) {
        use_ssl = true;
        url_part = balance_endpoint.substr(8);
    } else if (balance_endpoint.starts_with("http://")) {
        url_part = balance_endpoint.substr(7);
    } else {
        throw std::runtime_error("Invalid URL: " + balance_endpoint);
    }

    std::string host, path;
    auto slash_pos = url_part.find('/');
    if (slash_pos != std::string::npos) {
        host = url_part.substr(0, slash_pos);
        path = url_part.substr(slash_pos);
    } else {
        host = url_part;
        path = "/";
    }

    httplib::Client client((use_ssl ? "https://" : "http://") + host);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(10, 0);

    httplib::Headers headers = {
        {"Authorization", "Bearer " + target->api_key},
        {"Accept", "application/json"}
    };

    auto http_res = client.Get(path, headers);
    if (!http_res) {
        throw std::runtime_error("HTTP request failed: " + httplib::to_string(http_res.error()));
    }

    if (http_res->status != 200) {
        throw std::runtime_error("Balance API returned HTTP " + std::to_string(http_res->status)
                                 + ": " + http_res->body.substr(0, 200));
    }

    try {
        auto j = json::parse(http_res->body);
        json result;
        result["provider"] = provider_name;
        result["balance"] = j;
        return result;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Failed to parse balance response: " + std::string(e.what()));
    }
}

// =============================================================================
// Provider 解析 + LLM 调用
// =============================================================================
std::shared_ptr<LLMProvider> CodisServer::resolve_provider(const ChatRequest& req) {
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
std::string CodisServer::call_llm(const ChatRequest& req) {
    auto prov = resolve_provider(req);
    if (!prov) throw std::runtime_error("No provider configured. Set API key env var (e.g. GLM_API_KEY)");
    LOG_INFO("LLM call: provider='{}' model='{}' (req.model='{}')",
             prov->name(), prov->get_model(), req.model);
    auto result = prov->chat(req);
    if (!result.success) {
        LOG_ERROR("LLM call failed: {}", result.error);
        throw std::runtime_error(result.error);
    }
    return result.content;
}
} // namespace codis
