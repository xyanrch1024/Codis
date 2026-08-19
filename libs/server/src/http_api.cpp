#include "http_api.h"
#include "context_utils.h"
#include "log.h"

namespace codis {

HttpApi::HttpApi(Deps deps) : deps_(std::move(deps)) {}

void HttpApi::set_cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

void HttpApi::register_routes(httplib::Server& srv) {
    srv.Options("/api/v1/.*", [](const auto&, auto& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
        res.status = 204;
    });
    srv.Get("/api/v1/health",       [this](auto& r, auto& s) { handle_health(r, s); });
    srv.Get("/api/v1/info",         [this](auto& r, auto& s) { handle_info(r, s); });
    srv.Post("/api/v1/chat",        [this](auto& r, auto& s) { handle_chat(r, s); });
    srv.Post("/api/v1/sessions",    [this](auto& r, auto& s) { handle_session_create(r, s); });
    srv.Get("/api/v1/sessions",     [this](auto& r, auto& s) { handle_session_list(r, s); });
    srv.Get(R"(/api/v1/sessions/([a-f0-9\-]+))",     [this](auto& r, auto& s) { handle_session_get(r, s); });
    srv.Delete("/api/v1/sessions",            [this](auto& r, auto& s) { handle_session_delete_all(r, s); });
    srv.Delete(R"(/api/v1/sessions/([a-f0-9\-]+))", [this](auto& r, auto& s) { handle_session_delete(r, s); });
    srv.Post(R"(/api/v1/sessions/([a-f0-9\-]+)/messages)", [this](auto& r, auto& s) { handle_session_add_message(r, s); });
    srv.Get(R"(/api/v1/balance/([a-zA-Z0-9_\-]+))", [this](auto& r, auto& s) { handle_balance(r, s); });
}

void HttpApi::handle_health(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    json j;
    j["status"] = "ok";
    j["version"] = "0.5.0";
    j["protocol"] = "acp";
    j["port"] = deps_.port;
    j["default_provider"] = deps_.providers.default_name();
    j["tools"] = deps_.tools.list();
    res.set_content(json_dump_safe(j, 2), "application/json");
}

void HttpApi::handle_info(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    json j;
    j["providers"] = deps_.providers.list();
    j["default_provider"] = deps_.providers.default_name();
    json pm = json::object();
    for (auto& p : deps_.config.providers) pm[p.name] = p.model;
    j["provider_models"] = pm;
    j["tools"] = deps_.tools.list();
    j["features"] = {"acp", "chat", "websocket", "tools", "sessions"};
    j["skills"] = deps_.skills ? deps_.skills() : json::array();
    j["mcp_servers"] = deps_.mcp_status ? deps_.mcp_status() : json::array();
    res.set_content(json_dump_safe(j, 2), "application/json");
}

void HttpApi::handle_chat(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto body = json::parse(req.body);
        auto chat_req = ChatRequest::from_json(body);

        json tools = json::array();
        for (auto& s : deps_.tools.all_schemas()) {
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
        if (!chat_req.session_id.empty() && deps_.store.load_session(chat_req.session_id)) {
            auto baseline = deps_.system.build_baseline(chat_req.session_id, deps_.store);
            std::vector<Message> msgs;
            msgs.push_back({"system", baseline});
            auto history = deps_.store.load_messages(chat_req.session_id);
            auto replay = context_utils::replayable_messages(history);
            for (auto& m : replay)
                msgs.push_back(m);
            for (auto it = chat_req.messages.rbegin(); it != chat_req.messages.rend(); ++it)
                if (it->role == "user" && !it->content.empty()) {
                    msgs.push_back(*it);
                    break;
                }
            chat_req.messages = std::move(msgs);
        }

        std::string result = deps_.loop.call_llm(chat_req);

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

void HttpApi::handle_balance(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    std::string provider_name = req.matches[1];

    try {
        auto result = deps_.balance.query(deps_.config, provider_name);
        res.set_content(result.dump(2), "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

void HttpApi::handle_session_create(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    auto id = deps_.store.create_session();
    res.status = 201;
    res.set_content(json{{"session_id", id}}.dump(), "application/json");
}

void HttpApi::handle_session_list(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    auto sessions = deps_.store.list_sessions_info();
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

void HttpApi::handle_session_get(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    auto session = deps_.store.load_session(req.matches[1]);
    if (!session) { res.status = 404; res.set_content(R"({"error":"not found"})", "application/json"); return; }
    json j;
    j["id"] = session->id;
    auto msgs = deps_.store.load_messages(session->id);
    j["messages"] = json::array();
    for (auto& m : msgs) j["messages"].push_back(m.to_json());
    res.set_content(json_dump_safe(j, 2), "application/json");
}

void HttpApi::handle_session_delete(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    deps_.store.delete_session(req.matches[1]);
    res.set_content(R"({"status":"ok"})", "application/json");
}

void HttpApi::handle_session_delete_all(const httplib::Request&, httplib::Response& res) {
    set_cors(res);
    deps_.store.delete_all_sessions();
    res.set_content(R"({"status":"ok","deleted":"all"})", "application/json");
}

void HttpApi::handle_session_add_message(const httplib::Request& req, httplib::Response& res) {
    set_cors(res);
    try {
        auto msg = Message::from_json(json::parse(req.body));
        deps_.store.append_message(req.matches[1], msg);
        res.set_content(R"({"status":"ok"})", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

} // namespace codis