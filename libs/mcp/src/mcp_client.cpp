#include "mcp_client.h"

#include "log.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <httplib.h>

namespace codis::mcp {
namespace {

std::string json_str(const json& j) { return j.dump(); }

// https://host[:port]/path → scheme/host/path
struct UrlParts { std::string scheme, host, path; };

UrlParts split_url(const std::string& url) {
    UrlParts u;
    if (url.starts_with("https://")) { u.scheme = "https"; u.host = url.substr(8); }
    else if (url.starts_with("http://")) { u.scheme = "http"; u.host = url.substr(7); }
    else return {};
    auto slash = u.host.find('/');
    if (slash != std::string::npos) {
        u.path = u.host.substr(slash);
        u.host = u.host.substr(0, slash);
    } else {
        u.path = "/";
    }
    return u;
}

// SSE 响应体（event: message\ndata: {...}）→ 拼接 data 行解析为 JSON
json parse_sse_body(const std::string& body) {
    std::string data;
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("data:")) {
            if (!data.empty()) data += "\n";
            data += line.substr(5);
        }
    }
    return json::parse(data);
}

} // namespace

McpClient::McpClient(McpServerOptions opts) : opts_(std::move(opts)) {}

McpClient::~McpClient() { stop(); }

bool McpClient::spawn_stdio() {
    std::vector<std::string> argv0;
    argv0.push_back(opts_.command);
    for (auto& a : opts_.args) argv0.push_back(a);

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        LOG_ERROR("mcp[{}]: pipe failed: {}", opts_.name, strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        LOG_ERROR("mcp[{}]: fork failed: {}", opts_.name, strerror(errno));
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }
    if (pid == 0) {
        // 子进程：stdin ← in_pipe[0]，stdout → out_pipe[1]
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        for (auto& kv : opts_.env) {
            auto eq = kv.find('=');
            if (eq != std::string::npos && eq > 0)
                setenv(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), 1);
        }
        std::vector<char*> argv;
        for (auto& a : argv0) argv.push_back(a.data());
        argv.push_back(nullptr);
        execvp(argv0[0].c_str(), argv.data());
        _exit(127);  // exec 失败
    }

    // 父进程
    close(in_pipe[0]);
    close(out_pipe[1]);
    stdin_fd_ = in_pipe[1];
    stdout_fd_ = out_pipe[0];
    pid_ = pid;
    return true;
}

void McpClient::close_stdio() {
    if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    if (pid_ > 0) {
        int status = 0;
        waitpid(pid_, &status, WNOHANG);
        kill(pid_, SIGKILL);
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    if (reader_.joinable()) reader_.join();
}

bool McpClient::start() {
    stop();

    if (opts_.transport == "http") {
        auto parts = split_url(opts_.url);
        if (parts.host.empty()) {
            LOG_ERROR("mcp[{}]: invalid url '{}'", opts_.name, opts_.url);
            return false;
        }
        auto client = std::make_unique<httplib::Client>((parts.scheme + "://" + parts.host));
        client->set_connection_timeout(opts_.timeout_seconds, 0);
        client->set_read_timeout(opts_.timeout_seconds, 0);
        client->set_write_timeout(opts_.timeout_seconds, 0);
        http_ = std::move(client);
    } else if (opts_.transport == "stdio") {
        if (opts_.command.empty()) {
            LOG_ERROR("mcp[{}]: stdio requires command", opts_.name);
            return false;
        }
        if (!spawn_stdio()) return false;
        running_ = true;
        reader_ = std::thread([this] { reader_loop(); });
    } else {
        LOG_ERROR("mcp[{}]: unknown transport '{}'", opts_.name, opts_.transport);
        return false;
    }

    // initialize（协商协议版本）
    try {
        json init = request_impl("initialize", {
            {"protocolVersion", protocol_version_},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "codis"}, {"version", "0.1"}}}
        }, opts_.timeout_seconds * 1000);
        if (init.contains("protocolVersion"))
            protocol_version_ = init["protocolVersion"].get<std::string>();
        if (init.contains("instructions") && init["instructions"].is_string())
            LOG_INFO("mcp[{}]: server instructions: {}", opts_.name,
                     init["instructions"].get<std::string>().substr(0, 200));
        notify("notifications/initialized", json::object());
        running_ = true;
        LOG_INFO("mcp[{}]: initialized (protocol {})", opts_.name, protocol_version_);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("mcp[{}]: initialize failed: {}", opts_.name, e.what());
        stop();
        return false;
    }
}

void McpClient::stop() {
    if (!running_ && stdin_fd_ < 0 && !http_) return;
    running_ = false;
    {
        std::lock_guard lock(req_mutex_);
        auto pend = std::exchange(pending_, {});
        for (auto& [_, p] : pend)
            p.set_value(json{{"error", {{"code", -1}, {"message", "mcp client stopped"}}}});
    }
    if (opts_.transport == "stdio") close_stdio();
    http_.reset();
}

void McpClient::reader_loop() {
    std::string buf;
    char tmp[4096];
    while (running_) {
        ssize_t n = read(stdout_fd_, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, (size_t)n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            try {
                json j = json::parse(line);
                if (j.contains("id") && j["id"].is_number()) {
                    int64_t id = j["id"].get<int64_t>();
                    std::promise<json> p;
                    {
                        std::lock_guard lock(req_mutex_);
                        auto it = pending_.find(id);
                        if (it == pending_.end()) continue;
                        p = std::move(it->second);
                        pending_.erase(it);
                    }
                    p.set_value(std::move(j));
                } else {
                    // 服务端主动消息（请求/通知）：暂不处理
                    LOG_WARN("mcp[{}]: unexpected server message: {}", opts_.name,
                             line.substr(0, 200));
                }
            } catch (const json::parse_error&) {
                LOG_WARN("mcp[{}]: non-JSON line ignored", opts_.name);
            }
        }
    }
    running_ = false;
    {
        std::lock_guard lock(req_mutex_);
        auto pend = std::exchange(pending_, {});
        for (auto& [_, p] : pend)
            p.set_value(json{{"error", {{"code", -1}, {"message", "mcp server disconnected"}}}});
    }
    LOG_WARN("mcp[{}]: connection closed (pid={})", opts_.name, (int)pid_);
    if (on_disconnect && opts_.transport == "stdio") {
        try { on_disconnect(); } catch (const std::exception& e) {
            LOG_ERROR("mcp[{}]: on_disconnect threw: {}", opts_.name, e.what());
        }
    }
}

void McpClient::write_line(const std::string& line) {
    std::lock_guard lock(write_mutex_);
    if (stdin_fd_ < 0) throw std::runtime_error("mcp: stdin closed");
    std::string msg = line + "\n";
    if (write(stdin_fd_, msg.data(), msg.size()) != (ssize_t)msg.size())
        throw std::runtime_error("mcp: write failed: " + std::string(strerror(errno)));
}

std::string McpClient::http_request(const std::string& method, const json& params,
                                    int64_t id, int timeout_ms) {
    if (!http_) throw std::runtime_error("mcp: http client not started");
    auto parts = split_url(opts_.url);
    if (parts.host.empty()) throw std::runtime_error("mcp: invalid url");

    json msg = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
    if (id >= 0) msg["id"] = id;  // request 必须有 id；notification 无 id
    httplib::Headers headers = {
        {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", protocol_version_}
    };
    if (!session_id_.empty()) headers.emplace("mcp-session-id", session_id_);
    if (!opts_.bearer_token.empty()) headers.emplace("Authorization", "Bearer " + opts_.bearer_token);
    LOG_DEBUG("mcp[{}]: http POST {} ({} bytes)", opts_.name, parts.path, msg.dump().size());

    auto res = http_->Post(parts.path, headers, msg.dump(), "application/json");
    if (!res) throw std::runtime_error("mcp http: " + httplib::to_string(res.error()));
    if (res->status != 200 && res->status != 202)
        throw std::runtime_error("mcp http: status " + std::to_string(res->status) +
                                 " " + res->body.substr(0, 200));
    if (auto sid = res->get_header_value("mcp-session-id"); !sid.empty()) session_id_ = sid;

    // notification 等可能返回空 body（204/202 无内容）——不算错误
    if (res->body.empty()) return json::object().dump();

    std::string ct = res->get_header_value("Content-Type");
    json j;
    try {
        if (ct.find("text/event-stream") != std::string::npos) {
            j = parse_sse_body(res->body);
        } else {
            j = json::parse(res->body);
        }
    } catch (const json::parse_error& e) {
        throw std::runtime_error("mcp http: bad JSON response: " + std::string(e.what()));
    }
    if (j.contains("error"))
        throw std::runtime_error("mcp error " + std::to_string(j["error"].value("code", 0)) + ": " +
                                 j["error"].value("message", ""));
    return j.value("result", json::object()).dump();
}

json McpClient::request_impl(const std::string& method, const json& params, int timeout_ms) {
    std::string result_str;
    int64_t id = next_id_++;
    if (opts_.transport == "http") {
        result_str = http_request(method, params, id, timeout_ms);
        return json::parse(result_str);
    }

    std::promise<json> prom;
    auto fut = prom.get_future();
    {
        std::lock_guard lock(req_mutex_);
        pending_[id] = std::move(prom);
    }
    try {
        write_line(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump());
    } catch (...) {
        std::lock_guard lock(req_mutex_);
        pending_.erase(id);
        throw;
    }

    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        std::lock_guard lock(req_mutex_);
        pending_.erase(id);
        throw std::runtime_error("mcp: request timeout: " + method);
    }
    json j = fut.get();
    if (j.contains("error"))
        throw std::runtime_error("mcp error " + std::to_string(j["error"].value("code", 0)) + ": " +
                                 j["error"].value("message", ""));
    return j.value("result", json::object());
}

json McpClient::request(const std::string& method, const json& params) {
    if (!running_ && opts_.transport == "stdio") throw std::runtime_error("mcp: not running");
    return request_impl(method, params, opts_.timeout_seconds * 1000);
}

void McpClient::notify(const std::string& method, const json& params) {
    if (opts_.transport == "http") {
        try {
            http_request(method, params, -1, opts_.timeout_seconds * 1000);
        } catch (const std::exception& e) {
            LOG_WARN("mcp[{}]: notify {} failed: {}\n", opts_.name, method, e.what());
        }
        return;
    }
    try {
        write_line(json{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}.dump());
    } catch (const std::exception& e) {
        LOG_WARN("mcp[{}]: notify {} failed: {}", opts_.name, method, e.what());
    }
}

bool McpClient::list_tools(std::vector<McpToolInfo>* out) {
    try {
        std::string cursor;
        while (true) {
            json params = json::object();
            if (!cursor.empty()) params["cursor"] = cursor;
            json r = request("tools/list", params);
            if (r.contains("tools") && r["tools"].is_array()) {
                for (auto& t : r["tools"]) {
                    McpToolInfo info;
                    info.name = t.value("name", "");
                    info.description = t.value("description", "");
                    info.input_schema = t.value("inputSchema", json::object());
                    if (!info.name.empty()) out->push_back(std::move(info));
                }
            }
            if (r.contains("nextCursor") && r["nextCursor"].is_string()) {
                cursor = r["nextCursor"].get<std::string>();
                continue;
            }
            break;
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("mcp[{}]: tools/list failed: {}", opts_.name, e.what());
        return false;
    }
}

McpCallResult McpClient::call_tool(const std::string& name, const json& args) {
    McpCallResult res;
    json r = request("tools/call", {{"name", name}, {"arguments", args}});
    res.error = r.value("isError", false);
    if (r.contains("structuredContent")) res.structured = r["structuredContent"];
    if (r.contains("content") && r["content"].is_array()) {
        for (auto& c : r["content"]) {
            std::string type = c.value("type", "");
            if (type == "text") {
                if (!res.text.empty()) res.text += "\n";
                res.text += c.value("text", "");
            } else if (type == "image") {
                std::string data = c.value("data", "");
                std::string mime = c.value("mimeType", "unknown");
                res.text += "[image " + mime + " " + std::to_string(data.size()) + " bytes]";
            } else if (type == "resource") {
                res.text += "[embedded resource: " + c.value("uri", "") + "]";
            } else {
                res.text += "[content type: " + type + "]";
            }
        }
    }
    return res;
}

} // namespace codis::mcp