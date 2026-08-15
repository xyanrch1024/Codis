#include "tui.h"
#include "tui_tool_render.h"
#include "md_render.h"
#include "log.h"
#include "tool_format.h"
#include "clipboard.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <cstdlib>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <thread>
#include <unistd.h>

namespace codis {

using namespace ftxui;

// Sessions 面板的最大可见行数（固定面板高度，避免会话太多时撑满屏幕）
static constexpr int kMaxSessionRows = 12;

// 输入框的最大可见行数（超出后内部滚动，避免多行消息挤掉对话区）
static constexpr int kMaxInputRows = 6;

// Alt/Ctrl/Shift+Enter 换行（不同终端编码：xterm ESC+CR/LF、kitty/WezTerm 等 CSI-u）
static bool is_newline_key(const Event& ev) {
    const auto& s = ev.input();
    return s == "\x1b\r" || s == "\x1b\n" ||              // Alt+Enter（传统编码）
           s == "\x1b[13;2u" || s == "\x1b[13;3u" ||      // Shift+Enter / Alt+Enter
           s == "\x1b[13;4u" || s == "\x1b[13;5u" ||      // Shift+Alt / Ctrl+Enter
           s == "\x1b[13;6u" || s == "\x1b[13;7u";        // Ctrl+Shift / Ctrl+Alt
}

// 粘贴事件判定：bracketed paste 标记内，或与前一事件间隔 <50ms（快速连续）
static constexpr auto kPasteWindow = std::chrono::milliseconds(50);

// 支持的命令（用于 "/" 补全弹窗）
static const std::vector<std::pair<std::string, std::string>> kCommands = {
    {"/help", "Show help"},
    {"/exit", "Quit"},
    {"/clear", "Clear context"},
    {"/sessions", "List sessions"},
    {"/newsession", "New session"},
    {"/balance", "Check balance"},
    {"/model", "Switch model"},
    {"/clearsessions", "Delete all sessions"},
};

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
        if (s) {
            state_->current_session = *s;
            // 新会话：顶部展示启动 logo
            // 纯整格方块字形（与官网 codis.svg 同源）：不用 ╔╗╝║═ 框线字符，
            // 因为多数终端字体的框线字符只有半格高，与 █ 拼接会视觉断线。
            static constexpr const char* kBanner[] = {
                " ██████  ██████ ██████   ██ ███████",
                "██      ██    ████   ███ ██ ██      ",
                "██      ██    ████████ █ ██ ███████",
                "██      ██    ████   ███ ██      ██",
                " ██████  ██████ ██   ██  ██ ███████",
            };
            for (auto& line : kBanner)
                state_->add_item(ItemKind::Banner, line);
            state_->add_item(ItemKind::Status,
                             "Codis AI coding agent — type /help for commands");
        }
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

    // 任务完成后自动发送排队中的 pending 消息（Done/Error 触发）
    state_->on_idle_ = [this] { flush_pending(); };

    // 如果恢复已有 session，通过 REST 拉历史（SSE 长连接不推历史）
    if (!session_arg_.empty()) {
        auto info = acp_.get_session(state_->current_session);
        if (info) load_history(info->messages);
        // 首次 TUI 渲染时自动滚动到底部
        auto_scroll_ = true;
        scroll_px_ = 0;
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
    in_opt.placeholder = "";
    // 光标与 input_text 同步：Alt+Enter 手动插换行后，后续输入仍落在正确位置
    int cursor_pos = 0;
    in_opt.cursor_position = &cursor_pos;
    in_opt.multiline = true;
    in_opt.transform = [](InputState state) {
        if (state.is_placeholder) state.element |= dim;
        return state.element | bgcolor(Color(Color::Palette256::Grey7));
    };
    auto input = Input(std::move(in_opt));
    input |= CatchEvent([&](Event event) {
        // 粘贴检测（时序兜底，bracketed paste 标记外的场景）：
        // 与前一输入事件间隔 <50ms 视为快速连续流（粘贴）
        auto now = std::chrono::steady_clock::now();
        const bool rapid = now - last_event_at_ < kPasteWindow;
        // 粘贴流结束判定（>1s 无输入）：防止丢失 \x1b[201~ 后 in_paste_ 卡死
        const bool paste_done = now - last_event_at_ > std::chrono::seconds(1);
        last_event_at_ = now;
        if (in_paste_ && paste_done) in_paste_ = false;

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
            if ((event == Event::Tab ||
                 (event == Event::Return && !in_paste_ && !rapid)) &&
                !filtered.empty()) {
                if (cmd_selected_ >= (int)filtered.size()) cmd_selected_ = (int)filtered.size() - 1;
                auto& cmd = filtered[cmd_selected_].first;
                // 执行该命令
                send_message(cmd);
                input_text.clear();
                cursor_pos = 0;
                cmd_palette_visible_ = false;
                return true;
            }
            if (event == Event::Escape) {
                // 关闭补全：清空输入回到空状态
                input_text.clear();
                cursor_pos = 0;
                cmd_palette_visible_ = false;
                return true;
            }
            // 其它按键（继续输入）时保持弹窗，并更新选中索引
            if (event.is_character() && event.character().size() == 1) {
                cmd_selected_ = 0;
            }
        }

        // Alt/Ctrl/Shift+Enter：在光标处插入换行（多行输入）
        if (is_newline_key(event)) {
            cursor_pos = std::min(cursor_pos, (int)input_text.size());
            input_text.insert(cursor_pos, "\n");
            cursor_pos++;
            return true;
        }

        if (event == Event::Return) {
            if (in_paste_ || rapid) {
                // 粘贴内容里的换行：插入而不是发送
                cursor_pos = std::min(cursor_pos, (int)input_text.size());
                input_text.insert(cursor_pos, "\n");
                cursor_pos++;
                return true;
            }
            if (!input_text.empty()) {
                send_message(input_text);
                input_text.clear();
                cursor_pos = 0;
                return true;
            }
        }
        return false;
    });

    auto input_bar = Renderer(input, [&] {
        Elements els;
        // 命令补全弹窗（在 renderer 里计算可见性：此时 input_text 已更新）
        // 仅单行 "/" 前缀时显示，多行输入不再弹出
        cmd_palette_visible_ =
            input_text.starts_with("/") && input_text.find('\n') == std::string::npos;
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
        // 输入区高度随内容增长，但不超过 kMaxInputRows（超出后内部滚动）
        int input_lines = 1;
        for (char c : input_text)
            if (c == '\n') input_lines++;
        input_lines = std::clamp(input_lines, 1, kMaxInputRows);
        // pending 队列横幅（有排队消息时显示在输入框上方）
        if (int pc = state_->pending_count(); pc > 0) {
            auto ptext = " ⏳ pending: " + state_->pending_preview(2, 90);
            els.push_back(hbox({
                text(ptext) | color(Color::Yellow),
                flex(text("")),
                text(std::to_string(pc) + " queued") | dim,
            }) | bgcolor(Color(Color::Palette256::Grey7)));
        }
        els.push_back(vbox({text(""),
                            hbox({text("> "), input->Render() | flex |
                                                 size(HEIGHT, EQUAL, input_lines)}),
                            text("")}) | bgcolor(Color(Color::Palette256::Grey7)));
        return vbox(std::move(els));
    });

    // 对话区
    // UI 线程每帧：吞事件 → 逐条目构建逐行元素（行数 = 实际行数）。
    // row_sigs_/row_owner_ 与元素一一对应，供点击命中测试反查内容行。
    std::vector<std::string> row_sigs_;
    std::vector<int> row_owner_;
    auto conversation_view = Renderer([&] {
        // 单线程消费 WS 事件：先吞队列，再构建视图
        bool had = state_->drain_events();
        // 合并刷新后复位 Post 标志；若消费期间又有新事件到达，重新触发一次
        notify_pending = false;
        if (had && !state_->queue_empty())
            screen.Post(Event::Custom);

        int vw = std::max(10, Terminal::Size().dimx);
        int tw = vw - 2;  // 预留 vscroll_indicator 一列 + 1 列余量
        Elements els;
        auto card_bg = bgcolor(Color(Color::Palette256::Grey7));

        auto push_row = [&](Element el, std::string sig, int owner) {
            els.push_back(std::move(el));
            row_sigs_.push_back(std::move(sig));
            row_owner_.push_back(owner);
        };
        auto card_row = [&](Color bar_color, Element body, std::string body_sig, int owner) {
            push_row(hbox({text("┃ ") | color(bar_color) | bold, std::move(body) | flex}) | card_bg,
                     "┃ " + std::move(body_sig), owner);
        };

        row_sigs_.clear();
        row_owner_.clear();
        {
            bool first = true;
            for (int oi = 0; oi < (int)state_->items.size(); oi++) {
                auto& item = state_->items[oi];
                if (!first) push_row(text(""), "", oi);
                first = false;

                switch (item.kind) {
                case ItemKind::User:
                    for (auto& r : wrap_rows(item.text, tw - 2))
                        card_row(Color::Cyan, std::move(r.el) | color(Color::Cyan),
                                 std::move(r.sig), oi);
                    break;
                case ItemKind::Assistant: {
                    auto mrows = md_rows(item.text, tw - 2);
                    for (auto& r : mrows)
                        card_row(Color::Green,
                                 std::move(r.el) |
                                     (item.streaming ? color(Color::GreenLight)
                                                     : color(Color::Green)),
                                 std::move(r.sig), oi);
                    break;
                }
                case ItemKind::Reasoning:
                    for (auto& r : wrap_rows("· " + item.text, tw - 2, Color::GrayDark))
                        card_row(Color::GrayDark, std::move(r.el) | dim, std::move(r.sig), oi);
                    break;
                case ItemKind::ToolCall:
                    for (auto& r : render_tool_call(item, tw)) push_row(std::move(r.el),
                                                                        std::move(r.sig), oi);
                    break;
                case ItemKind::ToolResult:
                    for (auto& r : wrap_rows(item.text, tw, Color::GrayLight))
                        push_row(std::move(r.el), std::move(r.sig), oi);
                    break;
                case ItemKind::Error:
                    for (auto& r : wrap_rows(item.text, tw - 2))
                        card_row(Color::Red, std::move(r.el) | color(Color::Red),
                                 std::move(r.sig), oi);
                    break;
                case ItemKind::Status:
                    for (auto& r : wrap_rows(item.text, tw))
                        push_row(std::move(r.el) | dim, std::move(r.sig), oi);
                    break;
                case ItemKind::Banner:
                    push_row(text(item.text) | color(Color::Green) | bold,
                             item.text, oi);
                    break;
                }
            }
        }

        // 悬停的折叠目标行高亮（下划线），点击前即可确认可点
        if (hover_row_ >= 0 && hover_row_ < (int)els.size()) {
            int owner = row_owner_[hover_row_];
            if (owner >= 0 && owner < (int)state_->items.size()) {
                auto& it = state_->items[owner];
                if (it.kind == ItemKind::ToolCall && tool_foldable(it)) {
                    const std::string& sig = row_sigs_[hover_row_];
                    bool target = it.folded ? (sig == "  more...")
                                            : (sig.size() >= 3 && sig.rfind("▾ ", 0) == 0);
                    if (target) els[hover_row_] = els[hover_row_] | underlined;
                }
            }
        }

        Element content = vbox(std::move(els));

        // 限制内容宽度 ≤ 视口宽
        content = content | size(WIDTH, LESS_THAN, vw);

        // 行级滚动 → focusPositionRelative 比例
        int total_rows = (int)row_owner_.size();
        max_scroll_ = total_rows;
        if (auto_scroll_) {
            scroll_px_ = total_rows;
            content = content | focusPositionRelative(0.f, 1.f);
        } else {
            float frac = std::min(1.0f, (float)scroll_px_ / std::max(1, total_rows));
            content = content | focusPositionRelative(0.f, frac);
        }

        return content | frame | flex | vscroll_indicator;
    });

    // 屏幕坐标 → 对话内容行号。
    // 滚动偏移由 FTXUI frame/focusPositionRelative 的确定性公式直接算出
    // （已对照 v7.0.0 源码 frame.cpp/focus.cpp 并实证）：
    //   dy = clamp( int(total·y) − (VH−1)/2 , 0, max(total,VH) − VH )
    // 不再依赖二次渲染/像素匹配，点击命中不会因为匹配失败而静默失效。
    // 返回 -1 = 弹出层打开 / 不在对话区 / 超出内容范围。
    auto content_row_at = [&](int mx, int my) {
        if (help_visible_ || sessions_visible_ || cmd_palette_visible_) return -1;
        int dimy = Terminal::Size().dimy;
        int input_lines = 1;
        for (char c : input_text)
            if (c == '\n') input_lines++;
        input_lines = std::clamp(input_lines, 1, kMaxInputRows);
        int input_h = input_lines + 2 + (state_->pending_count() > 0 ? 1 : 0);
        // Header 2 行（Codis + 分隔线）+ 对话视口 + 输入区 + 状态栏 5 行
        const int conv_top = 2;
        const int conv_bottom = dimy - 5 - input_h;  // [conv_top, conv_bottom) 为对话视口
        if (my < conv_top || my >= conv_bottom) return -1;
        int VH = conv_bottom - conv_top;
        int total = (int)row_owner_.size();
        if (VH <= 0 || total == 0) return -1;
        float yfrac = auto_scroll_ ? 1.f : std::min(1.0f, (float)scroll_px_ / std::max(1, total));
        int dy = (int)((float)total * yfrac) - (VH - 1) / 2;
        dy = std::clamp(dy, 0, std::max(total, VH) - VH);
        int row = dy + (my - conv_top);
        return (row >= 0 && row < total) ? row : -1;
    };

    // 左键单击（无拖动）→ 命中折叠目标行则切换展开/截断
    auto toggle_fold_on_click = [&](int mx, int my) {
        int content_row = content_row_at(mx, my);
        if (content_row < 0) return;
        int owner = row_owner_[content_row];
        if (owner < 0 || owner >= (int)state_->items.size()) return;
        auto& it = state_->items[owner];
        if (it.kind != ItemKind::ToolCall || !tool_foldable(it)) return;
        // 截断态：仅 "  more..." 行可点击展开；展开态：仅 ▾ 命令行可点击收回
        const std::string& sig = row_sigs_[content_row];
        if (it.folded) {
            if (sig != "  more...") return;
        } else {
            if (sig.size() < 3 || sig.rfind("▾ ", 0) != 0) return;
        }
        it.folded = !it.folded;
        hover_row_ = -1;
        show_notice(it.folded ? "[Output collapsed]" : "[Output expanded]");
        LOG_DEBUG("fold toggle owner={} folded={} row={}", owner, it.folded, content_row);
    };

    auto main_container = Container::Vertical({conversation_view, input_bar});

    // 工作目录（进程 cwd，渲染期间不变；长路径截断避免撑破布局）
    std::string cwd;
    try {
        cwd = std::filesystem::current_path().string();
    } catch (...) {}
    if (cwd.size() > 50) cwd = "…" + cwd.substr(cwd.size() - 50);

    auto main_renderer = Renderer(main_container, [&] {
        // 瞬时提示过期清理（定时线程轮询 notice_pending_ 触发渲染）
        if (notice_pending_.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now - notice_at_ > std::chrono::seconds(3)) {
                notice_.clear();
                notice_pending_.store(false);
            }
        }

        // 底部状态栏（三行，状态在第二行）
        Elements status_lines;
        status_lines.push_back(hbox({
            text(acp_.connected() ? " ● Connected" : " ● Disconnected") |
                (acp_.connected() ? color(Color::Green) : color(Color::Red)),
            text("  │  " + model_) | dim,
            text("  │  context " + state_->context_size_str()) | dim,
            flex(text("")),
            text("  " + state_->current_session.substr(0, 8)) | dim | inverted,
        }) | bgcolor(Color(Color::Palette256::Grey7)));
        static constexpr const char* kSpinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
        static constexpr int kSpinnerCount = 10;
        if (state_->processing) spinner_frame_ = (spinner_frame_ + 1) % kSpinnerCount;
        status_lines.push_back(hbox({
            text(state_->processing
                     ? (std::string(" ") + kSpinner[spinner_frame_] + " processing...")
                     : std::string(" ● idle")) |
                (state_->processing ? color(Color::Yellow) : color(Color::Green)),
            flex(text("")),
            notice_.empty()
                ? text("")
                : text("  " + notice_ + " ") | color(Color::Yellow),
        }) | bgcolor(Color(Color::Palette256::Grey7)));
        status_lines.push_back(text("") | bgcolor(Color(Color::Palette256::Grey7)));
        status_lines.push_back(hbox({
            text(" " + cwd) | dim,
            flex(text("")),
        }) | bgcolor(Color(Color::Palette256::Grey7)));
        status_lines.push_back(text("") | bgcolor(Color(Color::Palette256::Grey7)));

        auto status_bar = vbox(std::move(status_lines));

        // Header: 居中 Codis + 分隔线 + 内容 + 状态栏
        Elements header = {
            text(" Codis ") | bold | center,
            separator(),
            main_container->Render() | flex,
            status_bar,
        };

        auto body = vbox(std::move(header));

        // Help overlay
        if (help_visible_) {
            Elements cmd_rows;
            for (const auto& [cmd, desc] : kCommands)
                cmd_rows.push_back(text("  " + cmd + "  " + desc));
            auto overlay = window(text(" Help "), vbox({
                text(" Commands:") | bold,
                vbox(std::move(cmd_rows)) | frame,
                separator(),
                text(" Keys:") | bold,
                text("  ESC ESC         cancel running task"),
                text("  Up/Down / wheel scroll conversation"),
                text("  Drag left mouse copy selected text"),
                separator(),
                text(" ESC to close ") | dim | center,
            })) | clear_under | center | border;
            return dbox({body, overlay});
        }

        // Sessions overlay
        if (sessions_visible_ && !session_list_.empty()) {
            Elements rows;
            for (int i = 0; i < (int)session_list_.size(); i++) {
                auto& s = session_list_[i];
                std::string prefix = (s.id == state_->current_session) ? "> " : "  ";
                auto el = text(prefix + s.id + "  " + std::to_string(s.message_count) + " msgs  " + s.title);
                if (s.id == state_->current_session) el = el | bold;
                if (i == session_selected_) el = el | inverted | focus;
                rows.push_back(el);
            }

            auto overlay = window(text(" Sessions "), vbox({
                vbox(std::move(rows)) | frame | size(HEIGHT, EQUAL, kMaxSessionRows) |
                    vscroll_indicator,
                separator(),
                 text(" " + std::to_string(session_selected_ + 1) + "/" +
                     std::to_string(session_list_.size()) + "  ↑↓/Tab  Enter(del)  ESC ") | dim | center,
            })) | clear_under | center | border;

            return dbox({body, overlay});
        }

        return body;
    });

    auto component = main_renderer | CatchEvent([&](Event event) {
        // bracketed paste 标记：\x1b[200~ 粘贴开始，\x1b[201~ 粘贴结束。
        // 期间的所有 Enter 视为换行（插入），否则多行粘贴会被当成多次 Enter 提前发送。
        // 这里必须最先处理，避免标记被当作普通按键。
        const auto& raw = event.input();
        if (raw == "\x1b[200~") {
            in_paste_ = true;
            return true;
        }
        if (raw == "\x1b[201~") {
            in_paste_ = false;
            return true;
        }

        // 左键按下/拖动/松开：高亮交给 FTXUI 内置选择处理，这里只跟踪拖拽状态；
        // 松开时若有实际拖动则直接用 GetSelection() 复制。
        // 注意：不能用 SelectionChange 回调——释放事件坐标通常等于最后一次移动，
        // selection_data_ 不变，回调不会触发，复制永远不会执行。
        if (event.is_mouse() && event.mouse().button == Mouse::Left) {
            if (event.mouse().motion == Mouse::Pressed) {
                drag_active_ = true;
                drag_moved_ = false;
                press_x_ = event.mouse().x;
                press_y_ = event.mouse().y;
            } else if (event.mouse().motion == Mouse::Moved) {
                // 位移累计 ≥3 格才算拖选（消除手抖误判为复制）
                if (drag_active_) {
                    int dist = std::abs(event.mouse().x - press_x_) +
                               std::abs(event.mouse().y - press_y_);
                    if (dist >= 3) drag_moved_ = true;
                }
                hover_row_ = content_row_at(event.mouse().x, event.mouse().y);
            } else if (event.mouse().motion == Mouse::Released) {
                if (drag_active_) {
                    drag_active_ = false;
                    if (drag_moved_) {
                        drag_moved_ = false;
                        const auto sel = screen.GetSelection();
                        if (!sel.empty()) {
                            copy_to_clipboard(sel);
                            show_notice("[Copied " + std::to_string(sel.size()) + " chars]");
                        }
                    } else {
                        // 无拖动的单击：命中折叠命令行则切换
                        toggle_fold_on_click(event.mouse().x, event.mouse().y);
                    }
                }
            }
            return false;
        }

        // ESC 键取消任务（双击）。必须在补全弹窗/输入组件之前处理：
        // 1) 补全弹窗的 CatchEvent 会吞掉第一个 ESC（清空输入并关闭弹窗），
        //    若在这里之后处理，双击永远只被看到一次；
        // 2) 快速双击（间隔 <50ms）会被 FTXUI 合并成 Event::Special("\x1b\x1b")，
        //    它 != Event::Escape，旧逻辑完全漏掉。
        // 做法：统计窗口内纯 "\x1b" 字节组成的按键次数（单个/合并都算），
        // 单次 ESC 仍向下传递（关闭补全/会话列表等），第二次才取消。
        {
            size_t esc_count = 0;
            const auto& in = event.input();
            while (esc_count < in.size() && in[esc_count] == '\x1b') esc_count++;
            if (esc_count > 0 && esc_count == in.size()) {
                if (state_->processing) {
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_escape_ > std::chrono::milliseconds(600))
                        esc_count_ = 0;
                    esc_count_ += static_cast<int>(esc_count);
                    last_escape_ = now;
                    if (esc_count_ >= 2) {
                        esc_count_ = 0;
                        last_escape_ = std::chrono::steady_clock::time_point{};
                        acp_.cancel_session(state_->current_session);
                        state_->processing = false;
                        state_->clear_pending();  // 取消时丢弃未发送的排队消息
                        show_notice("[Task cancelled]");
                        return true;
                    }
                } else {
                    // 非任务状态不累计，避免污染下次任务的首次按键
                    esc_count_ = 0;
                    last_escape_ = std::chrono::steady_clock::time_point{};
                }
            }
        }

        // 命令补全弹窗打开时：↑↓/ESC 交给输入组件处理，不滚动对话区/不触发取消
        if (cmd_palette_visible_) {
            if (event == Event::ArrowUp || event == Event::ArrowDown ||
                event == Event::Escape || event == Event::Tab || event == Event::Return)
                return false;
        }

        if (help_visible_) {
            if (event == Event::Escape) {
                help_visible_ = false;
                return true;
            }
            return true;  // 面板打开时吞掉其它按键（与 Sessions 一致）
        }

        if (sessions_visible_) {
            if (event == Event::Tab) {
                session_selected_ = (session_selected_ - 1 + (int)session_list_.size()) %
                                    (int)session_list_.size();
                return true;
            }
            if (event == Event::TabReverse) {
                session_selected_ = (session_selected_ + 1) % (int)session_list_.size();
                return true;
            }
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
                            acp_.switch_session(*sid);  // WS 移到新 session
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

        // 对话区上下滚动（行级）
        if (event == Event::ArrowUp) {
            if (auto_scroll_) {
                auto_scroll_ = false;
                scroll_px_ = std::max(0, max_scroll_ - 1);
            } else if (scroll_px_ > 0) {
                scroll_px_--;
            }
            post_job_();
            return true;
        }
        if (event == Event::ArrowDown) {
            if (!auto_scroll_) {
                scroll_px_++;
                // 一旦超过文档顶部，自动回到 auto-scroll
                if (scroll_px_ > 0 && scroll_px_ >= max_scroll_ - 2) {
                    scroll_px_ = 0;
                    auto_scroll_ = true;
                }
            }
            post_job_();
            return true;
        }

        // 鼠标滚轮滚动（行级，滚轮一次 3 行）
        if (event.is_mouse() && event.mouse().button == Mouse::WheelUp) {
            if (auto_scroll_) {
                auto_scroll_ = false;
                scroll_px_ = std::max(0, max_scroll_ - 3);
            } else {
                scroll_px_ = std::max(0, scroll_px_ - 3);
            }
            post_job_();
            return true;
        }
        if (event.is_mouse() && event.mouse().button == Mouse::WheelDown) {
            if (!auto_scroll_) {
                scroll_px_ += 3;
                if (scroll_px_ >= max_scroll_ - 2) {
                    scroll_px_ = 0;
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
    screen.TrackMouse(true);  // 开启鼠标追踪：滚轮滚动 + 左键拖拽选择复制
    exit_loop_ = screen.ExitLoopClosure();

    // 瞬时提示自动消失 + spinner 动画驱动：提示激活或 processing 时周期性触发渲染
    std::atomic<bool> timer_stop{false};
    std::thread notice_timer([&] {
        while (!timer_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (notice_pending_.load() || state_->processing) screen.Post(Event::Custom);
        }
    });

    // 开启 bracketed paste：终端会用 \x1b[200~ ... \x1b[201~ 包裹粘贴内容，
    // 据此识别粘贴，避免多行粘贴被当成多次 Enter 提前发送
    (void)::write(STDOUT_FILENO, "\x1b[?2004h", 8);

    screen.Loop(component);

    // 关闭 bracketed paste，恢复终端粘贴行为
    (void)::write(STDOUT_FILENO, "\x1b[?2004l", 8);

    timer_stop.store(true);
    notice_timer.join();

    acp_.disconnect();
    return 0;
}

void TuiClient::show_notice(const std::string& msg) {
    notice_ = msg;
    notice_at_ = std::chrono::steady_clock::now();
    notice_pending_.store(true);
    if (post_job_) post_job_();
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
    if (text == "/help") {
        help_visible_ = true;
        return;
    }
    if (text.starts_with("/newsession")) {
        auto sid = acp_.create_session();
        if (sid) {
            state_->clear_all();
            state_->current_session = *sid;
            acp_.switch_session(*sid);  // WS 移到新 session，否则服务端广播无连接可发
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

    // 当前任务处理中：新消息仅入 pending 队列，任务完成后自动发送
    if (state_->processing) {
        state_->pending_queue.push_back(text);
        auto_scroll_ = true;
        scroll_px_ = 0;
        post_job_();
        return;
    }
    send_request(text);
}

void TuiClient::send_request(const std::string& text) {
    state_->add_item(ItemKind::User, text);
    state_->processing = true;
    state_->request_start_ = std::chrono::steady_clock::now();
    state_->current_model_ = model_;
    auto_scroll_ = true;
    scroll_px_ = 0;
    post_job_();

    state_->history.push_back({"user", text});

    // 上下文由服务端从 SQLite 重建（session 历史 + baseline + tools），
    // 客户端只发当前这一条 user 消息，整段 history 不再重复传输。
    std::vector<Message> msgs;
    msgs.push_back({"user", text});

    ChatRequest req;
    req.model = model_;
    req.provider = provider_;
    req.messages = msgs;
    req.max_tokens = 4096;
    req.session_id = state_->current_session;

    acp_.send_async(req);
}

void TuiClient::flush_pending() {
    if (state_->processing) return;
    if (state_->pending_queue.empty()) return;
    std::string text = std::move(state_->pending_queue.front());
    state_->pending_queue.pop_front();
    send_request(text);
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
            auto j = codis::json::parse(http_res->body);
            state_->add_item(ItemKind::Status, "[Error] " + j.value("error", http_res->body));
        } catch (...) {
            state_->add_item(ItemKind::Status, "[Error] HTTP " + std::to_string(http_res->status) + ": " + http_res->body.substr(0, 200));
        }
        return;
    }

    try {
        auto j = codis::json::parse(http_res->body);
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

// 历史回放：把持久化的消息恢复成 TUI 条目。
// 与实时流一致——user/assistant 纯文本进 history（供构建请求），
// reasoning/tool 仅展示（不入 history，避免把思维链或工具角色发回模型）。
void TuiClient::load_history(const std::vector<Message>& msgs) {
    for (auto& m : msgs) {
        if (m.role == "user") {
            state_->add_item(ItemKind::User, m.content);
            state_->history.push_back(m);
        } else if (m.role == "reasoning") {
            state_->add_item(ItemKind::Reasoning, m.content);
        } else if (m.role == "tool") {
            // 合并进匹配的 ToolCall；未匹配则保底为独立条目
            bool matched = false;
            for (auto it = state_->items.rbegin(); it != state_->items.rend(); ++it) {
                if (it->kind == ItemKind::ToolCall && m.tool_call_id && it->tool_id == *m.tool_call_id) {
                    it->has_result = true;
                    it->tool_success = true;
                    it->result_text = m.content;
                    it->folded = tool_auto_fold(*it);
                    matched = true;
                    break;
                }
            }
            if (!matched)
                state_->add_item(ItemKind::ToolResult, m.content);
        } else if (m.role == "assistant") {
            if (m.tool_call_id) {
                json args = m.tool_arguments ? *m.tool_arguments : json::object();
                auto d = tool_display(m.tool_name.value_or(""), args);
                ConvItem item;
                item.kind = ItemKind::ToolCall;
                item.tool_id = *m.tool_call_id;
                item.tool_name = m.tool_name.value_or("");
                item.tool_icon = d.icon;
                item.text = d.label;
                item.tool_pending = d.pending;
                item.tool_title = d.block_title;
                item.tool_block = d.block;
                if (item.tool_name == "write" || item.tool_name == "edit")
                    item.content_text = format_tool_call(item.tool_name, args);
                state_->items.push_back(std::move(item));
                if (state_->notify_) state_->notify_();
            } else {
                state_->add_item(ItemKind::Assistant, m.content);
                state_->history.push_back(m);
            }
        }
    }
}

void TuiClient::switch_session(const SessionInfo& s) {
    state_->current_session = s.id;
    sessions_visible_ = false;

    // 切换到新 session 的 SSE（历史通过 REST 加载）
    acp_.switch_session(s.id);

    // 通过 REST API 拉历史
    auto info = acp_.get_session(s.id);
    state_->clear_all();
    if (info) load_history(info->messages);
    auto_scroll_ = true;
    scroll_px_ = 0;
    state_->add_item(ItemKind::Status, "[Session: " + s.id + "]");
    if (post_job_) post_job_();
}

} // namespace codis
