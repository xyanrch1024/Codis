#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <cstdlib>
#include <format>
#include <thread>
#include <chrono>
#include <filesystem>
#include <vector>

#include "acp_client.h"
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

bool ensure_server_running(int port, const std::string& server_binary) {
    if (is_server_running(port)) return true;

#ifdef __linux__
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(server_binary.c_str(), server_binary.c_str(), "-p", std::to_string(port).c_str(), nullptr);
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

    std::string model         = "gpt-4o";
    std::string prompt_arg;
    std::string system_prompt = "You are a helpful AI coding assistant.";
    bool interactive = false;
    int  max_tokens  = 4096;
    double temperature = 0.7;
    int  server_port = 8711;
    std::string server_bin;

    app.add_option("-p,--port",        server_port,   "Server port (default: 8711)");
    app.add_option("--server-bin",     server_bin,    "Server binary path");
    app.add_option("-m,--model",       model,         "Model name");
    app.add_option("-s,--system",      system_prompt, "System prompt");
    app.add_option("-t,--max-tokens",  max_tokens,    "Max tokens");
    app.add_option("--temperature",    temperature,   "Temperature");
    app.add_option("prompt",           prompt_arg,    "User prompt");
    app.add_flag("-i,--interactive",   interactive,   "Interactive mode");

    CLI11_PARSE(app, argc, argv);

    // 自动启动 server
    if (!is_server_running(server_port)) {
        LOG_INFO("server not running on port {}, attempting auto-start", server_port);
        std::string bin = server_bin.empty()
            ? (std::filesystem::path(argv[0]).parent_path() / "opencode-server").string()
            : server_bin;

        if (std::filesystem::exists(bin)) {
            ensure_server_running(server_port, bin);
        } else {
            LOG_ERROR("server binary not found: {}", bin);
            std::cerr << "Server not running. Start it: opencode-server -p " << server_port << "\n";
            return 1;
        }
    }

    if (!is_server_running(server_port)) {
        LOG_ERROR("server unreachable at localhost:{}", server_port);
        return 1;
    }

    AcpClient acp(server_port);

    // 构建请求模板
    auto build_req = [&](const std::vector<Message>& msgs) {
        ChatRequest req;
        req.model       = model;
        req.messages    = msgs;
        req.max_tokens  = max_tokens;
        req.temperature = temperature;
        req.stream      = true;
        return req;
    };

    // ---- 交互模式 ----
    if (interactive || prompt_arg.empty()) {
        std::cout << "╔══════════════════════════════════════════╗\n";
        std::cout << "║  OpenCode C++ Client  v0.3.0 (ACP)     ║\n";
        std::cout << std::format("║  Server:  localhost:{:<20d} ║\n", server_port);
        std::cout << std::format("║  Model:   {:<27s} ║\n", model);
        std::cout << std::format("║  Proto:   ACP + SSE                    ║\n");
        std::cout << "╚══════════════════════════════════════════╝\n";
        std::cout << "Type your message (Ctrl+D or /exit to quit):\n\n";

        std::vector<Message> conversation;
        conversation.push_back({"system", system_prompt});

        std::string line;
        while (true) {
            std::cout << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            if (line.empty()) continue;
            if (line == "/exit" || line == "/quit") break;

            conversation.push_back({"user", line});

            std::cout << "\n";
            std::string assistant_content;

            auto req = build_req(conversation);

            AcpClient::Callbacks cbs{
                .on_assistant = [&](std::string_view delta) {
                    std::cout << delta << std::flush;
                    assistant_content.append(delta);
                },
                .on_tool_call = [](const acp::ToolCallEvent& tc) {
                    std::cout << "\n[Tool: " << tc.name << "]\n";
                },
                .on_tool_result = [](const acp::ToolResultEvent& tr) {
                    std::cout << "[Result: " << (tr.success ? "ok" : "fail")
                              << "]\n";
                },
                .on_error = [](std::string_view msg) {
                    std::cerr << "\nError: " << msg << "\n";
                },
                .on_done = []() {}
            };

            bool ok = acp.send(req, cbs);

            std::cout << "\n\n";

            if (ok && !assistant_content.empty()) {
                conversation.push_back({"assistant", assistant_content});
            }
        }
    }
    // ---- 单次模式 ----
    else {
        std::vector<Message> messages = {
            {"system", system_prompt},
            {"user", prompt_arg}
        };
        auto req = build_req(messages);

        AcpClient::Callbacks cbs{
            .on_assistant = [](std::string_view delta) {
                std::cout << delta << std::flush;
            },
            .on_error = [](std::string_view msg) {
                std::cerr << "Error: " << msg << std::endl;
            }
        };

        if (!acp.send(req, cbs)) return 1;
        std::cout << std::endl;
    }

    return 0;
}
