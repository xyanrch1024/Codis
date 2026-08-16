#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <filesystem>

#include "acp_client.h"
#include "tui.h"
#include "log.h"

#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#endif

using namespace codis;

namespace {

bool is_server_running(int port) {
    AcpClient client(port);
    return client.health_check();
}

bool ensure_server_running(int port, const std::string& server_binary, const std::string& config_path) {
    if (is_server_running(port)) return true;

#ifdef __linux__
    pid_t pid = fork();
    if (pid == 0) {
        // daemon 化：不继承终端 fd，避免 CLI 退出/终端关闭后 server 挂死
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        auto port_str = std::to_string(port);
        execl(server_binary.c_str(), server_binary.c_str(), "-p", port_str.c_str(),
              "-c", config_path.c_str(), nullptr);
        _exit(127);
    }
    if (pid < 0) return false;

    for (int i = 0; i < 50; ++i) {
        if (is_server_running(port)) {
            std::cerr << "Server started (pid " << pid << ")\n";
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    kill(pid, SIGTERM);
    return false;
#else
    return false;
#endif
}

} // anonymous namespace

int main(int argc, char** argv) {
    CLI::App app{"Codis C++ Client — ACP + SSE (v0.3.0)"};

    std::string model         = "glm-4.5-flash";
    std::string provider      = "glm";
    int  server_port = 8711;
    std::string server_bin;
    std::string session_arg;
    bool clear_sessions = false;
    bool cont_session = false;
    bool auto_approve = false;

    app.add_flag("--clear-sessions",   clear_sessions, "Delete all sessions");
    app.add_flag("-c,--continue",      cont_session,   "Continue last session");
    app.add_flag("-y,--yes",           auto_approve,   "Auto-approve tool confirmations");
    app.add_option("-p,--port",        server_port,   "Server port (default: 8711)");
    app.add_option("--server-bin",     server_bin,    "Server binary path");
    app.add_option("-m,--model",       model,         "Model name");
    app.add_option("-S,--session",     session_arg,   "Session ID to attach");

    CLI11_PARSE(app, argc, argv);

    // 自动启动 server
    if (!is_server_running(server_port)) {
        LOG_INFO("server not running on port {}, attempting auto-start", server_port);

        // Linux 下用 /proc/self/exe 解析真实二进制路径（argv[0] 不可靠）
        std::filesystem::path exe = std::filesystem::canonical("/proc/self/exe");
        std::filesystem::path exe_dir = exe.parent_path();  // build/bin 或 /usr/local/bin

        std::filesystem::path bin = server_bin.empty()
            ? exe_dir / "codis-server"
            : std::filesystem::path(server_bin);

        if (!std::filesystem::exists(bin)) {
            LOG_ERROR("server binary not found: {}", bin.string());
            std::cerr << "Server not running. Start it: codis-server -p " << server_port << " -c config/config.toml\n";
            return 1;
        }

        // 配置文件探测（按优先级）：
        //   1. 已安装：  <bin>/../etc/codis/config.toml   (/usr/local/etc/codis/config.toml)
        //   2. 本地构建：<bin>/../../config/config.toml   (build/bin/../../config/config.toml)
        //   3. 兜底：   相对当前目录 config/config.toml
        std::filesystem::path config_path;
        for (auto cand : { exe_dir / "../etc/codis/config.toml",
                           exe_dir / "../../config/config.toml",
                           std::filesystem::path("config/config.toml") }) {
            auto p = cand.lexically_normal();
            if (std::filesystem::exists(p)) { config_path = p; break; }
        }
        if (config_path.empty()) {
            LOG_ERROR("config file not found (searched etc/codis and project config/)");
            return 1;
        }

        if (ensure_server_running(server_port, bin.string(), config_path.string()))
            LOG_INFO("server auto-started, config: {}", config_path.string());
        else
            LOG_ERROR("failed to start server: {}", bin.string());
    }

    if (!is_server_running(server_port)) {
        LOG_ERROR("server unreachable at localhost:{}", server_port);
        return 1;
    }

    AcpClient acp(server_port);

    if (clear_sessions) {
        if (acp.delete_all_sessions()) {
            std::cout << "All sessions deleted.\n";
        } else {
            std::cerr << "Failed to delete sessions.\n";
        }
        return 0;
    }

    // TUI 是唯一交互界面：默认启动，可选续接上次会话
    if (session_arg.empty() && cont_session)
        session_arg = acp.get_last_session();
    TuiClient tui(server_port, model, provider, session_arg, auto_approve);
    return tui.run();
}
