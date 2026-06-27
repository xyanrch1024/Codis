#include "tui.h"
#include "log.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <thread>
#include <chrono>
#include <cstdlib>

namespace opencode {

using namespace ftxui;

TuiClient::TuiClient(int server_port, std::string model, std::string provider,
                     std::string session_arg)
    : server_port_(server_port)
    , model_(std::move(model))
    , provider_(std::move(provider))
    , session_arg_(std::move(session_arg))
    , acp_(server_port)
    , state_(std::make_shared<TuiState>())
{
    state_->model = model_;
    state_->server_port = server_port_;
}

int TuiClient::run() {
    auto screen = ScreenInteractive::Fullscreen();

    if (!session_arg_.empty()) {
        state_->current_session = session_arg_;
    } else {
        auto s = acp_.create_session();
        if (s) state_->current_session = *s;
    }
    if (state_->current_session.empty()) {
        LOG_ERROR("Failed to create session");
        return 1;
    }

    // SSE 连接（后台线程）
    AcpClient::Callbacks cbs{
        .on_assistant = [this](std::string_view delta) {
            state_->append_pending(std::string(delta));
        },
        .on_tool_call = [this](const acp::ToolCallEvent& tc) {
            state_->add_line("[Tool: " + tc.name + "]");
        },
        .on_tool_result = [this](const acp::ToolResultEvent& tr) {
            state_->add_line("[Result: " + std::string(tr.success ? "ok" : "fail") + "]");
        },
        .on_error = [this](std::string_view msg) {
            state_->add_line("[Error: " + std::string(msg) + "]");
            state_->processing = false;
            state_->dirty = true;
        },
        .on_done = [this]() {
            state_->flush_pending();
        }
    };
    acp_.connect(state_->current_session, cbs);

    // 输入组件
    std::string input_text;
    auto input = Input(&input_text, "> ");
    input |= CatchEvent([&](Event event) {
        if (event == Event::Return && !input_text.empty()) {
            send_message(input_text);
            input_text.clear();
            return true;
        }
        return false;
    });

    auto input_bar = Renderer(input, [&] {
        return hbox({text("> "), input->Render() | flex}) | border;
    });

    // 对话区
    auto conversation_view = Renderer([&] {
        Elements els;
        {
            std::lock_guard lk(state_->mutex);
            for (auto& line : state_->lines) {
                if (line.starts_with("You: "))
                    els.push_back(text(line) | color(Color::Cyan));
                else if (line.starts_with("AI: "))
                    els.push_back(text(line) | color(Color::Green));
                else if (line.starts_with("[Tool"))
                    els.push_back(text(line) | color(Color::Yellow) | dim);
                else if (line.starts_with("[Result"))
                    els.push_back(text(line) | color(Color::Yellow));
                else if (line.starts_with("[Error"))
                    els.push_back(text(line) | color(Color::Red));
                else
                    els.push_back(text(line));
            }
            if (!state_->pending.empty())
                els.push_back(text("AI: " + state_->pending) | color(Color::GreenLight));
        }
        return vbox(std::move(els)) | frame | flex;
    });

    auto main_container = Container::Vertical({conversation_view, input_bar});

    auto main_renderer = Renderer(main_container, [&] {
        auto status = state_->processing ? " [processing...]" : "";
        return vbox({
            hbox({
                text(" Codis TUI ") | bold,
                text(" Model: " + model_ + status) | dim,
                text(" S: " + state_->current_session.substr(0, 8)) | dim,
                flex(text("")),
                text("/exit /clear /sessions") | dim,
            }),
            separator(),
            main_container->Render() | flex,
        }) | border;
    });

    auto component = main_renderer | CatchEvent([&](Event event) {
        if (event == Event::CtrlC) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    // 后台线程：SSE 有新数据时触发 FTXUI 重绘
    std::atomic<bool> refresh_running{true};
    std::thread refresh_thread([&] {
        while (refresh_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (state_->dirty.exchange(false))
                screen.Post([&] {});
        }
    });

    screen.Loop(component);

    refresh_running = false;
    if (refresh_thread.joinable()) refresh_thread.join();
    acp_.disconnect();
    return 0;
}

void TuiClient::send_message(const std::string& text) {
    if (text == "/exit") {
        std::exit(0);
    }
    if (text == "/clear") {
        cmd_clear();
        return;
    }
    if (text == "/sessions") {
        cmd_sessions();
        return;
    }
    if (text == "/clearsessions") {
        cmd_delete_all();
        return;
    }

    state_->add_line("You: " + text);
    state_->processing = true;
    state_->dirty = true;

    std::vector<Message> msgs;
    msgs.push_back({"system", state_->system_prompt});
    msgs.push_back({"user", text});

    ChatRequest req;
    req.model = model_;
    req.provider = provider_;
    req.messages = msgs;
    req.max_tokens = 4096;
    req.session_id = state_->current_session;

    acp_.send_async(req);
}

void TuiClient::cmd_sessions() {
    auto sessions = acp_.list_sessions();
    state_->add_line("--- Sessions ---");
    for (auto& s : sessions)
        state_->add_line("  " + s.id.substr(0, 8) + "  msgs:" + std::to_string(s.message_count) + "  " + s.title);
}

void TuiClient::cmd_clear() {
    std::lock_guard lk(state_->mutex);
    state_->lines.clear();
    state_->pending.clear();
    state_->dirty = true;
}

void TuiClient::cmd_delete_all() {
    acp_.delete_all_sessions();
    cmd_clear();
    state_->add_line("[All sessions deleted]");
}

} // namespace opencode
