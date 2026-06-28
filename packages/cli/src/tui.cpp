#include "tui.h"
#include "log.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <cstdlib>
#include <sstream>

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

    // SSE 连接
    state_->notify_ = [&screen] { screen.Post(Event::Custom); };
    post_job_ = state_->notify_;
    connect_sse();

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
            for (size_t i = 0; i < state_->lines.size(); i++) {
                auto& line = state_->lines[i];
                Element el = text(line);
                if (line.starts_with("You: "))
                    el = el | color(Color::Cyan);
                else if (line.starts_with("AI: "))
                    el = el | color(Color::Green);
                else if (line.starts_with("[Tool"))
                    el = el | color(Color::Yellow) | dim;
                else if (line.starts_with("[Result"))
                    el = el | color(Color::Yellow);
                else if (line.starts_with("[Error"))
                    el = el | color(Color::Red);
                els.push_back(std::move(el));
            }
            if (!state_->pending.empty())
                els.push_back(text("AI: " + state_->pending) | color(Color::GreenLight));

            // 焦点控制：frame 将滚动到带 focus 的元素
            if (!els.empty()) {
                if (auto_scroll_) {
                    els.back() = els.back() | focus;
                } else if (scroll_line_ >= 0 && scroll_line_ < (int)els.size()) {
                    els[scroll_line_] = els[scroll_line_] | focus;
                }
            }
        }
        return vbox(std::move(els)) | frame | flex | vscroll_indicator;
    });

    auto main_container = Container::Vertical({conversation_view, input_bar});

    auto main_renderer = Renderer(main_container, [&] {
        auto status = state_->processing ? " [processing...]" : "";

        Elements header = {
            hbox({
                text(" Codis TUI ") | bold,
                text(" Model: " + model_ + status) | dim,
                text(" S: " + state_->current_session) | dim,
                flex(text("")),
                text("/exit /clear /sessions /newsession /balance") | dim,
            }),
            separator(),
            main_container->Render() | flex,
        };

        // Sessions overlay
        if (sessions_visible_ && !session_list_.empty()) {
            Elements rows;
            for (int i = 0; i < (int)session_list_.size(); i++) {
                auto& s = session_list_[i];
                auto line = s.id + "  " + std::to_string(s.message_count) + " msgs  " + s.title;
                auto el = text("  " + line);
                if (i == session_selected_) el = el | inverted;
                if (s.id == state_->current_session) el = text("> " + line) | bold;
                rows.push_back(el);
            }

            auto overlay = window(text(" Sessions "), vbox({
                vbox(std::move(rows)) | frame | flex,
                separator(),
                 text(" " + std::to_string(session_selected_ + 1) + "/" +
                     std::to_string(session_list_.size()) + "  ↑↓  Enter(del)  ESC ") | dim | center,
            })) | clear_under | center | border;

            return dbox({vbox(std::move(header)) | border, overlay});
        }

        return vbox(std::move(header)) | border;
    });

    auto component = main_renderer | CatchEvent([&](Event event) {
        if (sessions_visible_) {
            if (event == Event::ArrowUp && session_selected_ > 0) {
                session_selected_--;
                return true;
            }
            if (event == Event::ArrowDown && session_selected_ < (int)session_list_.size() - 1) {
                session_selected_++;
                return true;
            }
            if ((event == Event::d || event == Event::D) && !session_list_.empty()) {
                auto& s = session_list_[session_selected_];
                bool was_current = (s.id == state_->current_session);
                acp_.delete_session(s.id);
                session_list_ = acp_.list_sessions();
                if (session_list_.empty()) {
                    sessions_visible_ = false;
                    state_->add_line("[Last session deleted, creating new...]");
                    auto sid = acp_.create_session();
                    if (sid) {
                        state_->lines.clear();
                        state_->pending.clear();
                        state_->history.clear();
                        state_->current_session = *sid;
                    }
                } else {
                    session_selected_ = std::min(session_selected_, (int)session_list_.size() - 1);
                    if (was_current) {
                        auto sid = acp_.create_session();
                        if (sid) {
                            state_->lines.clear();
                            state_->pending.clear();
                            state_->history.clear();
                            state_->current_session = *sid;
                            state_->add_line("[Session " + s.id + " deleted, new session created]");
                        }
                    }
                }
                return true;
            }
            if (event == Event::Return && !session_list_.empty()) {
                switch_session(session_list_[session_selected_]);
                return true;
            }
            if (event == Event::Escape) {
                sessions_visible_ = false;
                return true;
            }
            return true;
        }

        // 对话区上下滚动
        if (event == Event::ArrowUp) {
            if (auto_scroll_) {
                auto_scroll_ = false;
                scroll_line_ = (int)state_->lines.size();
                if (!state_->pending.empty()) scroll_line_++;
                scroll_line_ = std::max(0, scroll_line_ - 1);
            } else if (scroll_line_ > 0) {
                scroll_line_--;
            }
            post_job_();
            return true;
        }
        if (event == Event::ArrowDown) {
            if (scroll_line_ >= 0) {
                scroll_line_++;
                int max_line = (int)state_->lines.size();
                if (!state_->pending.empty()) max_line++;
                if (scroll_line_ >= max_line) {
                    scroll_line_ = -1;
                    auto_scroll_ = true;
                }
            }
            post_job_();
            return true;
        }

        if (event == Event::CtrlS) {
            session_list_ = acp_.list_sessions();
            session_selected_ = 0;
            sessions_visible_ = true;
            return true;
        }

        if (event == Event::CtrlC) {
            screen.ExitLoopClosure()();
            return true;
        }
        return false;
    });

    screen.Loop(component);

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
        session_list_ = acp_.list_sessions();
        session_selected_ = 0;
        sessions_visible_ = true;
        return;
    }
    if (text == "/clearsessions") {
        cmd_delete_all();
        return;
    }
    if (text.starts_with("/newsession")) {
        auto sid = acp_.create_session();
        if (sid) {
            state_->lines.clear();
            state_->pending.clear();
            state_->history.clear();
            state_->current_session = *sid;
            state_->add_line("[New session created: " + *sid + "]");
        }
        return;
    }
    if (text.starts_with("/balance")) {
        cmd_balance(text);
        return;
    }

    state_->add_line("You: " + text);
    state_->processing = true;
    auto_scroll_ = true;
    scroll_line_ = -1;
    post_job_();

    state_->history.push_back({"user", text});

    std::vector<Message> msgs;
    msgs.push_back({"system", state_->system_prompt});
    for (auto& m : state_->history) msgs.push_back(m);

    ChatRequest req;
    req.model = model_;
    req.provider = provider_;
    req.messages = msgs;
    req.max_tokens = 4096;
    req.session_id = state_->current_session;

    acp_.send_async(req);
}

void TuiClient::cmd_clear() {
    std::lock_guard lk(state_->mutex);
    state_->lines.clear();
    state_->pending.clear();
    state_->history.clear();
    if (post_job_) post_job_();
}

void TuiClient::cmd_delete_all() {
    acp_.delete_all_sessions();
    cmd_clear();
    state_->add_line("[All sessions deleted]");
    if (post_job_) post_job_();
}

void TuiClient::cmd_balance(const std::string& line) {
    std::string prov = "deepseek";
    auto parts = [&]() {
        std::vector<std::string> v;
        std::istringstream iss(line);
        std::string w;
        while (iss >> w) v.push_back(w);
        return v;
    }();
    if (parts.size() > 1) prov = parts[1];

    httplib::Client client("127.0.0.1", server_port_);
    client.set_connection_timeout(10, 0);
    client.set_read_timeout(10, 0);

    auto http_res = client.Get(("/api/v1/balance/" + prov).c_str());
    if (!http_res) {
        state_->add_line("[Error] Server unreachable: " + httplib::to_string(http_res.error()));
        return;
    }
    if (http_res->status != 200) {
        try {
            auto j = opencode::json::parse(http_res->body);
            state_->add_line("[Error] " + j.value("error", http_res->body));
        } catch (...) {
            state_->add_line("[Error] HTTP " + std::to_string(http_res->status) + ": " + http_res->body.substr(0, 200));
        }
        return;
    }

    try {
        auto j = opencode::json::parse(http_res->body);
        auto& bal = j["balance"];
        state_->add_line("--- " + prov + " Balance ---");

        if (bal.contains("balance_infos") && !bal["balance_infos"].empty()) {
            for (auto& bi : bal["balance_infos"]) {
                state_->add_line("  Total:   " + bi.value("total_balance", "N/A"));
                state_->add_line("  Topped:  " + bi.value("topped_up_balance", "N/A"));
                state_->add_line("  Granted: " + bi.value("granted_balance", "N/A"));
            }
        } else {
            state_->add_line("  Response: " + bal.dump(2));
        }

        if (bal.contains("is_available")) {
            state_->add_line("  Active:  " + std::string(bal["is_available"].get<bool>() ? "Yes" : "No"));
        }
    } catch (const std::exception& e) {
        state_->add_line("[Error] Parse failed: " + std::string(e.what()));
    }
}

AcpClient::Callbacks TuiClient::build_callbacks() {
    return {
        .on_assistant = [this](std::string_view delta) {
            LOG_DEBUG("SSE delta: {}", delta);
            state_->append_pending(std::string(delta));
            //state_->flush_pending();
        },
        .on_tool_call = [this](const acp::ToolCallEvent& tc) {
            LOG_DEBUG("SSE tool_call: {}", tc.name);
            state_->add_line("[Tool: " + tc.name + "]");
        },
        .on_tool_result = [this](const acp::ToolResultEvent& tr) {
            LOG_DEBUG("SSE tool_result: {} success={}", tr.content.substr(0, 100), tr.success);
            state_->add_line("[Result: " + std::string(tr.success ? "ok" : "fail") + "]");
        },
        .on_error = [this](std::string_view msg) {
            state_->add_line("[Error: " + std::string(msg) + "]");
            state_->processing = false;
        },
        .on_done = [this]() {
            state_->flush_pending();
        }
    };
}

void TuiClient::connect_sse() {
    acp_.connect(state_->current_session, build_callbacks());
}

void TuiClient::switch_session(const SessionInfo& s) {
    {
        std::lock_guard lk(state_->mutex);
        state_->lines.clear();
        state_->pending.clear();
        state_->history.clear();
        state_->lines.push_back("[Switching to session " + s.id + "...]");
    }
    state_->current_session = s.id;
    sessions_visible_ = false;
    if (post_job_) post_job_();
    // 通知服务端 SSE 切换到新 session，服务端推新历史
    acp_.switch_session(s.id);
}

} // namespace opencode
