#include "tui.h"
#include "tui_tool_render.h"
#include "log.h"
#include "tool_format.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <cstdlib>
#include <sstream>
#include <algorithm>

namespace opencode {

using namespace ftxui;

// 支持的命令（用于 "/" 补全弹窗）
static const std::vector<std::pair<std::string, std::string>> kCommands = {
    {"/exit", "退出"},
    {"/clear", "清空上下文"},
    {"/sessions", "会话列表"},
    {"/newsession", "新建会话"},
    {"/balance", "查询余额"},
    {"/model", "切换模型"},
    {"/clearsessions", "删除所有会话"},
};

// UTF-8 感知、按显示宽度换行：
//   - CJK 等宽字符按 2 列计算
//   - 优先在空格处断行；无空格的长句（中文）在超宽处硬断
//   - 行内已有文本时尽量在最后一个空格处回退断行
static std::vector<std::string> wrap_by_width(const std::string& text, int width) {
    std::vector<std::string> lines;
    if (width <= 0) return {text};
    std::string line;
    int line_w = 0;
    size_t i = 0;
    const size_t n = text.size();

    auto utf8_len = [](char c) {
        unsigned char u = (unsigned char)c;
        if (u >= 0xF0) return 4;
        if (u >= 0xE0) return 3;
        if (u >= 0xC0) return 2;
        return 1;
    };

    auto push_line = [&]() {
        lines.push_back(line);
        line.clear();
        line_w = 0;
    };

    while (i < n) {
        char c = text[i];
        if (c == '\n') {
            push_line();
            i++;
            continue;
        }
        int clen = utf8_len(c);
        if (i + clen > n) clen = 1;
        std::string_view cp(text.data() + i, clen);
        int w = string_width(cp);

        if (cp == " ") {
            if (!line.empty() && line_w + 1 <= width) {
                line += ' ';
                line_w += 1;
            }
            i += clen;
            continue;
        }

        if (line_w + w <= width) {
            line.append(cp);
            line_w += w;
            i += clen;
            continue;
        }

        // 超宽：优先回退到行内最后一个空格处断行（head 足够宽才有意义）
        if (!line.empty()) {
            auto sp = line.find_last_of(' ');
            if (sp != std::string::npos && string_width(line.substr(0, sp)) >= 3) {
                lines.push_back(line.substr(0, sp));
                line = line.substr(sp + 1) + std::string(cp);
                line_w = string_width(line);
                i += clen;
                continue;
            }
        }
        if (!line.empty()) {
            // 无空格可断：硬断（长单词 / 无空格中文）
            push_line();
            continue;  // 重新处理当前字符
        }
        // 单字符比整行宽：逐字硬断
        line.append(cp);
        line_w += w;
        if (line_w >= width) push_line();
        i += clen;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// 按宽度换行后转为 vbox<text> 元素
static Element wrapped_text(const std::string& content, int width) {
    Elements els;
    for (auto& ln : wrap_by_width(content, width)) els.push_back(ftxui::text(ln));
    return vbox(std::move(els));
}

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
    // 合并刷新：流式增量频繁触发 screen.Post，会淹没输入事件导致 ESC 等按键丢失。
    // 用原子标志确保同一时刻最多只有一个 Custom 事件在队列里。
    std::atomic<bool> notify_pending{false};
    state_->notify_ = [&screen, &notify_pending] {
        bool expected = false;
        if (notify_pending.compare_exchange_strong(expected, true))
            screen.Post(Event::Custom);
    };
    // Custom 事件被处理（渲染）后复位标志，允许下一次 Post
    post_job_ = [&screen, &notify_pending] {
        notify_pending = false;
        screen.Post(Event::Custom);
    };
    connect_sse();

    // 如果恢复已有 session，通过 REST 拉历史（SSE 长连接不推历史）
    if (!session_arg_.empty()) {
        auto info = acp_.get_session(state_->current_session);
        if (info) {
            for (auto& m : info->messages) {
                if (m.role == "user") {
                    state_->add_item(ItemKind::User, m.content);
                    state_->history.push_back(m);
                } else if (m.role == "assistant") {
                    state_->add_item(ItemKind::Assistant, m.content);
                    state_->history.push_back(m);
                }
            }
        }
        // 首次 TUI 渲染时自动滚动到底部
        auto_scroll_ = true;
        scroll_item_ = -1;
    }

    // 输入组件
    std::string input_text;
    // 过滤与已输入前缀匹配的命令（补全弹窗共享）
    auto filtered_commands = [&]() {
        std::vector<std::pair<std::string, std::string>> out;
        for (auto& [cmd, desc] : kCommands)
            if (input_text.empty() || cmd.starts_with(input_text)) out.push_back({cmd, desc});
        return out;
    };

    InputOption in_opt;
    in_opt.content = &input_text;
    in_opt.placeholder = "> ";
    in_opt.transform = [](InputState state) {
        if (state.is_placeholder) state.element |= dim;
        return state.element | bgcolor(Color(Color::Palette256::Grey19));
    };
    auto input = Input(std::move(in_opt));
    input |= CatchEvent([&](Event event) {
        // "/" 开头的输入：显示命令补全弹窗（可见性在 renderer 中按 input_text 计算）
        if (cmd_palette_visible_) {
            auto filtered = filtered_commands();

            if (event == Event::ArrowUp && cmd_selected_ > 0) {
                cmd_selected_--;
                return true;
            }
            if (event == Event::ArrowDown && cmd_selected_ < (int)filtered.size() - 1) {
                cmd_selected_++;
                return true;
            }
            if ((event == Event::Tab || event == Event::Return) && !filtered.empty()) {
                if (cmd_selected_ >= (int)filtered.size()) cmd_selected_ = (int)filtered.size() - 1;
                auto& cmd = filtered[cmd_selected_].first;
                // 执行该命令
                send_message(cmd);
                input_text.clear();
                cmd_palette_visible_ = false;
                return true;
            }
            if (event == Event::Escape) {
                // 关闭补全：清空输入回到空状态
                input_text.clear();
                cmd_palette_visible_ = false;
                return true;
            }
            // 其它按键（继续输入）时保持弹窗，并更新选中索引
            if (event.is_character() && event.character().size() == 1) {
                cmd_selected_ = 0;
            }
        }

        if (event == Event::Return && !input_text.empty()) {
            send_message(input_text);
            input_text.clear();
            return true;
        }
        return false;
    });

    auto input_bar = Renderer(input, [&] {
        Elements els;
        // 命令补全弹窗（在 renderer 里计算可见性：此时 input_text 已更新）
        cmd_palette_visible_ = input_text.starts_with("/");
        if (cmd_palette_visible_) {
            auto filtered = filtered_commands();
            if (!filtered.empty()) {
                if (cmd_selected_ >= (int)filtered.size()) cmd_selected_ = 0;
                Elements rows;
                for (int i = 0; i < (int)filtered.size(); i++) {
                    auto& [cmd, desc] = filtered[i];
                    auto el = text("  " + cmd) | flex;
                    if (i == cmd_selected_) el = el | inverted;
                    rows.push_back(hbox({el, text("  " + desc) | dim}));
                }
                els.push_back(window(text(" Commands "), vbox(std::move(rows)) | frame) |
                             clear_under | border);
            }
        }
        els.push_back(vbox({text(""),
                            hbox({text("> "), input->Render() | flex}),
                            text("")}) | bgcolor(Color(Color::Palette256::Grey19)));
        return vbox(std::move(els));
    });

    // 对话区
    auto conversation_view = Renderer([&] {
        // 单线程消费 WS 事件：先吞队列，再构建视图
        bool had = state_->drain_events();
        // 合并刷新后复位 Post 标志；若消费期间又有新事件到达，重新触发一次
        notify_pending = false;
        if (had && !state_->queue_empty())
            screen.Post(Event::Custom);

        Elements els;
        {
            int vw = std::max(10, Terminal::Size().dimx);
            int tw = vw - 2;  // 预留 vscroll_indicator 一列 + 1 列余量
            for (auto& item : state_->items) {
                Element el;
                switch (item.kind) {
                case ItemKind::User:
                    el = wrapped_text("❯ " + item.text, tw) | color(Color::Cyan);
                    break;
                case ItemKind::Assistant:
                    el = wrapped_text(item.text, tw) |
                         (item.streaming ? color(Color::GreenLight) : color(Color::Green));
                    break;
                case ItemKind::Reasoning: {
                    el = wrapped_text("· " + item.text, tw) | color(Color::GrayDark) | dim;
                    break;
                }
                case ItemKind::ToolCall:
                    el = render_tool_call(item);
                    break;
                case ItemKind::ToolResult: {
                    auto txt = opencode::truncate_tool_output(item.text);
                    el = wrapped_text(txt, tw) | color(Color::GrayLight);
                    break;
                }
                case ItemKind::Error:
                    el = wrapped_text(item.text, tw) | color(Color::Red);
                    break;
                case ItemKind::Status:
                    el = wrapped_text(item.text, tw) | dim;
                    break;
                }
                els.push_back(std::move(el));
            }
        }

        int total = (int)els.size();
        Element content = vbox(std::move(els));

        // 关键：限制内容宽度 ≤ 视口宽（终端宽）。否则 frame 会按内容自身
        // 自然宽度(min_x)分配画布，paragraph 在比视口宽的画布里不换行，
        // 后续换行行被横向裁剪不可见（表现为“输出截断”）。加上这个约束后
        // 段落按视口宽度换行，resize 时重渲染读取新宽度重新换行。
        int vw = std::max(10, Terminal::Size().dimx);
        content = content | size(WIDTH, LESS_THAN, vw);

        // focusPositionRelative 控制 frame 滚动到内容的指定比例位置
        // focus() 对单行 text() 元素无效（focused.box.y_min 始终为 0，dy=0）
        // focusPositionRelative(0, 1) 滚动到底部（auto-scroll）
        if (auto_scroll_ || scroll_item_ < 0) {
            content = content | focusPositionRelative(0.f, 1.f);
        } else {
            float frac = (float)scroll_item_ / std::max(1, total);
            content = content | focusPositionRelative(0.f, frac);
        }

        return content | frame | flex | vscroll_indicator;
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

            return dbox({vbox(std::move(header)), overlay});
        }

        return vbox(std::move(header));
    });

    auto component = main_renderer | CatchEvent([&](Event event) {
        // 命令补全弹窗打开时：↑↓/ESC 交给输入组件处理，不滚动对话区/不触发取消
        if (cmd_palette_visible_) {
            if (event == Event::ArrowUp || event == Event::ArrowDown ||
                event == Event::Escape || event == Event::Tab || event == Event::Return)
                return false;
        }

        // 双击 ESC（400ms 内两次）：取消正在执行的任务（不退程序）
        if (event == Event::Escape) {
            auto now = std::chrono::steady_clock::now();
            if (state_->processing &&
                now - last_escape_ <= std::chrono::milliseconds(400)) {
                last_escape_ = std::chrono::steady_clock::time_point{};
                acp_.cancel_session(state_->current_session);
                state_->processing = false;
                state_->add_item(ItemKind::Status, "[任务已取消]");
                post_job_();
                return true;
            }
            last_escape_ = now;
        }

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
                    auto sid = acp_.create_session();
                    if (sid) {
                        state_->clear_all();
                        state_->current_session = *sid;
                    }
                } else {
                    session_selected_ = std::min(session_selected_, (int)session_list_.size() - 1);
                    if (was_current) {
                        auto sid = acp_.create_session();
                        if (sid) {
                            state_->clear_all();
                            state_->current_session = *sid;
                            state_->add_item(ItemKind::Status, "[Session " + s.id + " deleted, new session created]");
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

        // 对话区上下滚动（按 item 索引）
        if (event == Event::ArrowUp) {
            if (auto_scroll_) {
                auto_scroll_ = false;
                scroll_item_ = std::max(0, (int)state_->items.size() - 1);
            } else if (scroll_item_ > 0) {
                scroll_item_--;
            }
            post_job_();
            return true;
        }
        if (event == Event::ArrowDown) {
            if (scroll_item_ >= 0) {
                scroll_item_++;
                if (scroll_item_ >= (int)state_->items.size()) {
                    scroll_item_ = -1;
                    auto_scroll_ = true;
                }
            }
            post_job_();
            return true;
        }

        // 鼠标滚轮滚动（与 ↑↓ 同语义）
        if (event.is_mouse() && event.mouse().button == Mouse::WheelUp) {
            if (auto_scroll_) {
                auto_scroll_ = false;
                scroll_item_ = std::max(0, (int)state_->items.size() - 1);
            } else if (scroll_item_ > 0) {
                scroll_item_--;
            }
            post_job_();
            return true;
        }
        if (event.is_mouse() && event.mouse().button == Mouse::WheelDown) {
            if (scroll_item_ >= 0) {
                scroll_item_++;
                if (scroll_item_ >= (int)state_->items.size()) {
                    scroll_item_ = -1;
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

    input->TakeFocus();
    screen.TrackMouse(true);  // 开启鼠标追踪：滚轮滚动生效；复制改用 Shift+拖拽
    exit_loop_ = screen.ExitLoopClosure();
    screen.Loop(component);

    acp_.disconnect();
    return 0;
}

void TuiClient::send_message(const std::string& text) {
    if (text == "/exit") {
        // 退出 FTXUI 事件循环，让 run() 走完 acp_.disconnect() + 终端恢复，
        // 而非 std::exit() 直接杀掉进程（后者会留下 raw 终端/挂起线程）
        if (exit_loop_) exit_loop_();
        return;
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
            state_->clear_all();
            state_->current_session = *sid;
            state_->add_item(ItemKind::Status, "[New session created: " + *sid + "]");
        }
        return;
    }
    if (text.starts_with("/balance")) {
        cmd_balance(text);
        return;
    }
    if (text.starts_with("/model")) {
        cmd_model(text);
        return;
    }

    state_->add_item(ItemKind::User, text);
    state_->processing = true;
    auto_scroll_ = true;
    scroll_item_ = -1;
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
    state_->clear_all();
    if (post_job_) post_job_();
}

void TuiClient::cmd_delete_all() {
    acp_.delete_all_sessions();
    cmd_clear();
    state_->add_item(ItemKind::Status, "[All sessions deleted]");
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
        state_->add_item(ItemKind::Status, "[Error] Server unreachable: " + httplib::to_string(http_res.error()));
        return;
    }
    if (http_res->status != 200) {
        try {
            auto j = opencode::json::parse(http_res->body);
            state_->add_item(ItemKind::Status, "[Error] " + j.value("error", http_res->body));
        } catch (...) {
            state_->add_item(ItemKind::Status, "[Error] HTTP " + std::to_string(http_res->status) + ": " + http_res->body.substr(0, 200));
        }
        return;
    }

    try {
        auto j = opencode::json::parse(http_res->body);
        auto& bal = j["balance"];
        state_->add_item(ItemKind::Status, "--- " + prov + " Balance ---");

        if (bal.contains("balance_infos") && !bal["balance_infos"].empty()) {
            for (auto& bi : bal["balance_infos"]) {
                state_->add_item(ItemKind::Status, "  Total:   " + bi.value("total_balance", "N/A"));
                state_->add_item(ItemKind::Status, "  Topped:  " + bi.value("topped_up_balance", "N/A"));
                state_->add_item(ItemKind::Status, "  Granted: " + bi.value("granted_balance", "N/A"));
            }
        } else {
            state_->add_item(ItemKind::Status, "  Response: " + bal.dump(2));
        }

        if (bal.contains("is_available")) {
            state_->add_item(ItemKind::Status, "  Active:  " + std::string(bal["is_available"].get<bool>() ? "Yes" : "No"));
        }
    } catch (const std::exception& e) {
        state_->add_item(ItemKind::Status, "[Error] Parse failed: " + std::string(e.what()));
    }
}

void TuiClient::cmd_model(const std::string& line) {
    auto parts = [&]() {
        std::vector<std::string> v;
        std::istringstream iss(line);
        std::string w;
        while (iss >> w) v.push_back(w);
        return v;
    }();

    auto info = acp_.get_server_info();
    if (!info) {
        state_->add_item(ItemKind::Status, "[Error] Cannot query server info (unreachable?)");
        if (post_job_) post_job_();
        return;
    }

    // /model <name> — 切换到指定 provider（使用其配置的 model）
    if (parts.size() >= 2) {
        const std::string& name = parts[1];
        auto it = std::find(info->providers.begin(), info->providers.end(), name);
        if (it == info->providers.end()) {
            state_->add_item(ItemKind::Status, "[Error] Unknown provider: " + name);
            if (post_job_) post_job_();
            return;
        }
        provider_ = name;
        auto mit = info->provider_models.find(name);
        if (mit != info->provider_models.end()) {
            model_ = mit->second;
            state_->model = model_;
        }
        state_->add_item(ItemKind::Status, "[Model switched to " + name +
                          (mit != info->provider_models.end() ? " (" + mit->second + ")" : "") + "]");
        if (post_job_) post_job_();
        return;
    }

    // /model — 列出当前 + 可用 provider
    state_->add_item(ItemKind::Status, "--- Model ---");
    state_->add_item(ItemKind::Status, "  Current: " + provider_ + " (" + model_ + ")");
    for (auto& p : info->providers) {
        auto mit = info->provider_models.find(p);
        std::string model = mit != info->provider_models.end() ? mit->second : "?";
        state_->add_item(ItemKind::Status, "  " + p + " → " + model +
                          (p == provider_ ? "  (current)" : "") +
                          (p == info->default_provider ? "  (default)" : ""));
    }
    state_->add_item(ItemKind::Status, "  Usage: /model <provider>");
    if (post_job_) post_job_();
}

AcpClient::Callbacks TuiClient::build_callbacks() {
    return {
        .on_assistant = [this](std::string_view delta) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::AssistantDelta;
            ev.text = std::string(delta);
            state_->push_event(ev);
        },
        .on_reasoning = [this](std::string_view delta) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::ReasoningDelta;
            ev.text = std::string(delta);
            state_->push_event(ev);
        },
        .on_tool_call = [this](const acp::ToolCallEvent& tc) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::ToolCall;
            ev.tool_call = tc;
            state_->push_event(ev);
        },
        .on_tool_result = [this](const acp::ToolResultEvent& tr) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::ToolResult;
            ev.tool_result = tr;
            state_->push_event(ev);
        },
        .on_error = [this](std::string_view msg) {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::Error;
            ev.text = std::string(msg);
            state_->push_event(ev);
        },
        .on_done = [this]() {
            AcpEvent ev;
            ev.kind = AcpEvent::Kind::Done;
            state_->push_event(ev);
        }
    };
}

void TuiClient::connect_sse() {
    acp_.connect(state_->current_session, build_callbacks());
}

void TuiClient::switch_session(const SessionInfo& s) {
    state_->current_session = s.id;
    sessions_visible_ = false;

    // 切换到新 session 的 SSE（历史通过 REST 加载）
    acp_.switch_session(s.id);

    // 通过 REST API 拉历史
    auto info = acp_.get_session(s.id);
    state_->clear_all();
    if (info) {
        for (auto& m : info->messages) {
            if (m.role == "user") {
                state_->add_item(ItemKind::User, m.content);
                state_->history.push_back(m);
            } else if (m.role == "assistant") {
                state_->add_item(ItemKind::Assistant, m.content);
                state_->history.push_back(m);
            }
        }
    }
    auto_scroll_ = true;
    scroll_item_ = -1;
    state_->add_item(ItemKind::Status, "[Session: " + s.id + "]");
    if (post_job_) post_job_();
}

} // namespace opencode
