#include <CLI/CLI.hpp>
#include <iostream>
#include <string>
#include <cstdlib>
#include <format>
#include <thread>
#include <chrono>
#include <filesystem>
#include <vector>
#include <sstream>

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

    std::string model         = "glm-4.5-flash";
    std::string provider      = "glm";
    std::string prompt_arg;
    std::string system_prompt = "You are a helpful AI coding assistant.";
    bool interactive = false;
    int  max_tokens  = 4096;
    double temperature = 0.7;
    int  server_port = 8711;
    std::string server_bin;
    std::string session_arg;

    app.add_option("-p,--port",        server_port,   "Server port (default: 8711)");
    app.add_option("--server-bin",     server_bin,    "Server binary path");
    app.add_option("-m,--model",       model,         "Model name");
    app.add_option("-S,--session",     session_arg,   "Session ID to attach");
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
    std::string current_session;
    auto build_req = [&](const std::vector<Message>& msgs) {
        ChatRequest req;
        req.model       = model;
        req.provider = provider;
        req.messages    = msgs;
        req.max_tokens  = max_tokens;
        req.temperature = temperature;
        req.stream      = true;
        req.session_id  = current_session;
        return req;
    };

    // ---- 会话初始化 ----
    if (!session_arg.empty()) {
        current_session = session_arg;
    } else {
        current_session = acp.get_last_session();
    }
    if (current_session.empty()) {
        auto sid = acp.create_session();
        if (sid) current_session = *sid;
    }

    // ---- 交互模式 ----
    if (interactive || prompt_arg.empty()) {
        std::vector<Message> conversation;
        conversation.push_back({"system", system_prompt});

        // 加载 session 历史
        if (!current_session.empty()) {
            auto info = acp.get_session(current_session);
            if (info) {
                conversation = info->messages;
            }
        }
        if (current_session.empty()) {
            auto sid = acp.create_session();
            if (sid) current_session = *sid;
        }

        auto show_header = [&]() {
            std::cout << "╔══════════════════════════════════════════╗\n";
            std::cout << "║  Codis Client  v0.6.0                  ║\n";
            std::cout << std::format("║  Server:   localhost:{:<20d} ║\n", server_port);
            std::cout << std::format("║  Model:    {:<27s} ║\n", model);
            if (!current_session.empty()) {
                auto short_id = current_session.substr(0, 8) + "...";
                std::cout << std::format("║  Session:  {:<27s} ║\n", short_id);
            }
            std::cout << "╚══════════════════════════════════════════╝\n";
        };
        show_header();

        // 显示历史
        if (conversation.size() > 1) {
            std::cout << std::format("── {} messages loaded from session ──\n", conversation.size() - 1);
            for (size_t i = 1; i < conversation.size(); i++) {
                auto& m = conversation[i];
                if (m.role == "user")  std::cout << "You: " << m.content << "\n";
                else if (m.role == "assistant") std::cout << "AI: " << m.content << "\n";
            }
            std::cout << "──────────────────────────────────────\n";
        } else {
            std::cout << "New session. Type your message.\n";
        }

        std::cout << "\nCommands: /exit /sessions /session <id> /clear\n\n";

        // 启动后台 SSE 监听（实时接收其他 client 的广播）
        AcpClient::Callbacks view_cbs{
            .on_assistant = [&](std::string_view delta) {
                std::cout << "\r\033[K" << delta << std::flush;  // 覆盖 > 行
            },
            .on_tool_call = [](const acp::ToolCallEvent& tc) {
                std::cout << "\n[Broadcast Tool: " << tc.name << "]\n";
            },
            .on_error = [](std::string_view msg) {
                std::cout << "\n[Broadcast Error: " << msg << "]\n";
            },
            .on_done = [&]() {
                std::cout << "\n\n> " << std::flush;
            }
        };
        if (!current_session.empty()) acp.connect(current_session, view_cbs);

        std::string line;
        while (true) {
            std::cout << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            if (line.empty()) continue;

            // ---- 特殊命令 ----
            if (line == "/exit" || line == "/quit") { acp.disconnect(); break; }

            if (line == "/sessions") {
                auto sessions = acp.list_sessions();
                if (sessions.empty()) {
                    std::cout << "No sessions found.\n\n";
                } else {
                    std::cout << std::format("{:<10} {:<5} {:<10} {}\n", "ID", "Msgs", "Active", "Title");
                    std::cout << std::string(60, '-') << "\n";
                    for (auto& s : sessions) {
                        auto short_id = s.id.substr(0, 8);
                        auto marker = (s.id == current_session) ? "*" : " ";
                        auto msg_count_str = std::to_string(s.message_count);
                        auto title = s.title.size() > 35 ? s.title.substr(0, 35) + "..." : s.title;
                        std::cout << std::format("{}{:<9} {:<5} {:<10} {}\n",
                            marker, short_id, msg_count_str, "", title);
                    }
                    std::cout << "  * = current session\n\n";
                }
                continue;
            }

            if (line.starts_with("/session ")) {
                auto parts = [&]() {
                    std::vector<std::string> v;
                    std::istringstream iss(line);
                    std::string w;
                    while (iss >> w) v.push_back(w);
                    return v;
                }();
                if (parts.size() < 2) { std::cout << "Usage: /session <id> [use|del]\n\n"; continue; }

                auto sid = parts[1];
                auto cmd = parts.size() > 2 ? parts[2] : "";

                if (cmd == "del" || cmd == "delete") {
                    if (acp.delete_session(sid)) {
                        std::cout << "Session deleted: " << sid.substr(0, 8) << "...\n\n";
                        if (sid == current_session) {
                            current_session.clear();
                            conversation.clear();
                            conversation.push_back({"system", system_prompt});
                        }
                    } else {
                        std::cout << "Failed to delete session.\n\n";
                    }
                    continue;
                }

                if (cmd == "use" || cmd == "switch" || cmd.empty()) {
                    auto info = acp.get_session(sid);
                    if (!info) {
                        std::cout << "Session not found.\n\n";
                        continue;
                    }
                    current_session = sid;
                    conversation = info->messages;
                    show_header();
                    std::cout << std::format("Restored session: {} messages loaded.\n\n",
                        conversation.size());
                    continue;
                }

                std::cout << "Usage: /session <id> [use|del]\n\n";
                continue;
            }

            if (line == "/clear") {
                conversation.clear();
                conversation.push_back({"system", system_prompt});
                if (!current_session.empty()) acp.create_session();
                std::cout << "Context cleared.\n\n";
                continue;
            }

            // ---- 普通消息 ----
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
                    std::cout << "[Result: " << (tr.success ? "ok" : "fail") << "]\n";
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
