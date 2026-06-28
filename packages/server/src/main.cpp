#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <optional>
#include <filesystem>
#include <signal.h>
#include <mutex>
#include <condition_variable>

#include <CLI/CLI.hpp>
#include "server.h"
#include "log.h"

static std::mutex g_signal_mtx;
static std::condition_variable g_signal_cv;
static bool g_shutdown = false;

extern "C" void signal_handler(int) {
    {
        std::lock_guard<std::mutex> lock(g_signal_mtx);
        g_shutdown = true;
    }
    g_signal_cv.notify_one();
}

int main(int argc, char** argv) {
    CLI::App app{"OpenCode Server — Multi-Provider Backend"};

    int port = 8711;
    std::string host = "127.0.0.1";
    std::string config_path;

    app.add_option("-p,--port",    port,        "Server port (default: 8711)");
    app.add_option("-H,--host",    host,        "Bind host");
    app.add_option("-c,--config",  config_path, "Config file (TOML)");

    CLI11_PARSE(app, argc, argv);

    LOG_INFO("OpenCode Server v0.3.1 starting (port {}, host {})", port, host);
    if (!config_path.empty()) LOG_INFO("config file: {}", config_path);

    std::optional<std::string> cfg;
    if (!config_path.empty()) {
        cfg = std::filesystem::absolute(config_path).string();
    }

    opencode::OpenCodeServer server(port, cfg);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    server.start();

    {
        std::unique_lock<std::mutex> lock(g_signal_mtx);
        g_signal_cv.wait(lock, [] { return g_shutdown; });
    }

    LOG_INFO("received shutdown signal");
    server.stop();
    return 0;
}
