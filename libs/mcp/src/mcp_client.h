#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

namespace codis::mcp {

using json = nlohmann::json;

// 一个 MCP 工具的描述（tools/list 返回项）
struct McpToolInfo {
    std::string name;
    std::string description;
    json input_schema;
};

// tools/call 的解析结果
struct McpCallResult {
    bool error = false;        // isError（工具级失败，非传输失败）
    std::string text;          // content 中 text 段拼接（image 等给占位描述）
    json structured;           // structuredContent（可能为空）
};

struct McpServerOptions {
    std::string name;                 // 配置里的唯一标识（冲突时作工具名前缀）
    std::string transport = "stdio";  // stdio | http
    std::string command;              // stdio: 可执行命令（npx / python / node ...）
    std::vector<std::string> args;    // stdio: 命令参数
    std::vector<std::string> env;     // stdio: 附加环境变量 "KEY=VALUE"
    std::string url;                  // http: https://host[:port]/path
    std::string bearer_token;         // http: Authorization: Bearer <token>（可空）
    int timeout_seconds = 30;
};

// MCP 客户端：JSON-RPC 2.0 会话（stdio 子进程或 Streamable HTTP）
// 线程安全：request/notify 可并发调用；stdio 由后台 reader 线程分发响应。
class McpClient {
public:
    explicit McpClient(McpServerOptions opts);
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    // 建立连接：stdio spawn / http 预检，initialize + initialized 通知
    bool start();
    void stop();
    bool running() const { return running_; }

    // 工具列表（自动处理分页 nextCursor）
    bool list_tools(std::vector<McpToolInfo>* out);
    // 调用工具；传输/协议错误抛 std::runtime_error，工具级失败放 McpCallResult::error
    McpCallResult call_tool(const std::string& name, const json& args);

    // 原始 JSON-RPC 请求/通知（resources/sampling 等扩展用）
    json request(const std::string& method, const json& params);
    void notify(const std::string& method, const json& params);

    // stdio 进程退出 / 传输断开回调（reader 线程触发，勿在其中长时间阻塞）
    std::function<void()> on_disconnect;

    const std::string& name() const { return opts_.name; }
    const std::string& transport() const { return opts_.transport; }

private:
    json request_impl(const std::string& method, const json& params, int timeout_ms);
    void write_line(const std::string& line);
    void reader_loop();
    bool spawn_stdio();
    void close_stdio();
    std::string http_request(const std::string& method, const json& params, int64_t id, int timeout_ms);

    McpServerOptions opts_;
    std::atomic<bool> running_{false};

    // http
    std::unique_ptr<httplib::Client> http_;

    // stdio
    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    std::thread reader_;
    std::mutex write_mutex_;

    // 请求-响应分发
    std::mutex req_mutex_;
    std::map<int64_t, std::promise<json>> pending_;
    int64_t next_id_ = 1;

    std::string protocol_version_ = "2025-06-18";
    std::string session_id_;  // http 会话
};

} // namespace codis::mcp