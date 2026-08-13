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
#endif

using namespace opencode;

namespace {

bool is_server_running(int port) {
    AcpClient client(port);
    return client.health_check();
}

bool ensure_server_running(int port, const std::string& server_binary, const std::string& project_root) {
    if (is_server_running(port)) return true;

#ifdef __linux__
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        chdir(project_root.c_str());
        execl(server_binary.c_str(), server_binary.c_str(), "-p", std::to_string(port).c_str(),
              "-c", "config/config.toml", nullptr);
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
    CLI::App app{"OpenCode C++ Client — ACP + SSE (v0.3.0)"};

    std::string model         = "glm-4.5-flash";
    std::string provider      = "glm";
    int  server_port = 8711;
    std::string server_bin;
    std::string session_arg;
    bool clear_sessions = false;
    bool cont_session = false;

    app.add_flag("--clear-sessions",   clear_sessions, "Delete all sessions");
    app.add_flag("-c,--continue",      cont_session,   "Continue last session");
    app.add_option("-p,--port",        server_port,   "Server port (default: 8711)");
    app.add_option("--server-bin",     server_bin,    "Server binary path");
    app.add_option("-m,--model",       model,         "Model name");
    app.add_option("-S,--session",     session_arg,   "Session ID to attach");

    CLI11_PARSE(app, argc, argv);

    // 自动启动 server
    if (!is_server_running(server_port)) {
        LOG_INFO("server not running on port {}, attempting auto-start", server_port);

        auto cli_dir = std::filesystem::path(argv[0]).parent_path();  // build/packages/cli
        auto project_root = std::filesystem::canonical(cli_dir / "../../..").string();

        std::string bin = server_bin.empty()
            ? (project_root + "/build/packages/server/opencode-server")
            : server_bin;

        if (std::filesystem::exists(bin)) {
            ensure_server_running(server_port, bin, project_root);
        } else {
            LOG_ERROR("server binary not found: {}", bin);
            std::cerr << "Server not running. Start it: opencode-server -p " << server_port << " -c config/config.toml\n";
            return 1;
        }
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
    TuiClient tui(server_port, model, provider, session_arg);
    return tui.run();
}
