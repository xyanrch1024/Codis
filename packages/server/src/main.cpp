#include <asio/signal_set.hpp>
#include <asio/io_context.hpp>

#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <optional>
#include <filesystem>

#include <CLI/CLI.hpp>
#include "server.h"
#include "log.h"

namespace asio = ::asio;

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

    asio::io_context ioc;
    asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&](auto ec, auto) {
        if (!ec) { LOG_INFO("received shutdown signal"); server.stop(); ioc.stop(); }
    });

    server.start();
    ioc.run();

    return 0;
}
