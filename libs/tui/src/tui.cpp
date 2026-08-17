#include "tui.h"
#include "tui_tool_render.h"
#include "clipboard.h"
#include "log.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <thread>
#include <unistd.h>

namespace codis {

using namespace ftxui;

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
    {"/yolo", "YOLO mode: auto-approve all tools"},
    {"/compact", "Compress context (LLM summary)"},
    {"/info", "Show skills & MCP servers"},
};

TuiClient::TuiClient(int server_port, std::string model, std::string provider,
                     std::string session_arg, bool auto_approve)
    : server_port_(server_port)
    , session_arg_(std::move(session_arg))
    , auto_approve_(auto_approve)
    , acp_(server_port)
    , state_(std::make_shared<TuiState>())
    , controller_(acp_, state_, std::move(model), std::move(provider), auto_approve, server_port)
{
    // 未显式指定 -m/-p 时，从 server 拉默认 provider 与模型（与配置一致）
    if (controller_.model().empty()) {
        auto info = acp_.get_server_info();
        if (info && !info->default_provider.empty()) {
            std::string default_model;
            auto it = info->provider_models.find(info->default_provider);
            if (it != info->provider_models.end()) default_model = it->second;
            controller_.set_model_provider(std::move(default_model), info->default_provider);
        }
    }
    state_->model = controller_.model();
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
    controller_.connect_sse();

    // 任务完成后自动发送排队中的 pending 消息（Done/Error 触发）
    state_->on_idle_ = [this] { controller_.flush_pending(); };

    // 如果恢复已有 session，通过 REST 拉历史（SSE 长连接不推历史）
    if (!session_arg_.empty()) {
        auto info = acp_.get_session(state_->current_session);
        if (info) controller_.load_history(info->messages);
        // 首次 TUI 渲染时自动滚动到底部
        auto_scroll_ = true;
        scroll_px_ = 0;
    }

    // ---- 业务层回调接线（视图效果的唯一入口）----
    UiCallbacks cb;
    cb.exit = [&] { if (exit_loop_) exit_loop_(); };
    cb.notify = [&] { post_job_(); };
    cb.notice = [this](const std::string& m) { show_notice(m); };
    cb.reset_scroll = [&] {
        auto_scroll_ = true;
        scroll_px_ = 0;
        post_job_();
    };
    cb.show_help = [&] { help_.visible = true; };
    cb.show_sessions = [&](std::vector<SessionInfo> list, bool reset_selection) {
        sessions_.list = std::move(list);
        if (reset_selection) sessions_.selected = 0;
        sessions_.selected = std::min(sessions_.selected,
                                      std::max(0, (int)sessions_.list.size() - 1));
        sessions_.visible = true;
    };
    cb.show_info = [&](std::vector<SkillBrief> sk, std::vector<McpServerBrief> mc) {
        info_.skills = std::move(sk);
        info_.mcps = std::move(mc);
        info_.sel[0] = info_.sel[1] = 0;
        info_.pane = 0;
        info_.visible = true;
    };
    cb.hide_sessions = [&] { sessions_.visible = false; };
    controller_.set_callbacks(std::move(cb));

    sessions_.on_activate = [this](const SessionInfo& s) { controller_.switch_session(s); };
    sessions_.on_delete = [this](const SessionInfo& s) { controller_.delete_session(s); };
    confirm_.on_respond = [this](bool approve) { respond_confirm(approve); };

    // 工作目录（进程 cwd，渲染期间不变；长路径截断避免撑破布局）
    try {
        cwd_ = std::filesystem::current_path().string();
    } catch (...) {}
    if (cwd_.size() > 50) cwd_ = "…" + cwd_.substr(cwd_.size() - 50);

    // ---- 输入组件 ----
    std::string input_text;
    // 粘贴检测状态（输入事件判定；bracketed paste 标记或时序兜底）
    bool in_paste_ = false;
    std::chrono::steady_clock::time_point last_event_at_;
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

    // 命令补全弹窗状态（仅 run() 内使用）
    bool cmd_palette_visible_ = false;
    int cmd_selected_ = 0;
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
                controller_.send_message(cmd);
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
                controller_.send_message(input_text);
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
                els.push_back(render_cmd_palette(filtered, cmd_selected_));
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

    // ---- 对话区 ----
    // UI 线程每帧：吞事件 → 构建逐行元素（视图层产出布局，本层负责滚动/命中）
    bool drag_active_ = false;   // 左键按下中
    bool drag_moved_ = false;    // 是否发生了实际拖动（区分点击/拖拽）
    int press_x_ = -1;           // 按下位置（位移阈值依据）
    int press_y_ = -1;
    int hover_row_ = -1;         // 悬停内容行（-1 = 无）
    int esc_count_ = 0;          // 窗口内累计的 ESC 次数（兼容合并的 "\x1b\x1b"）
    std::chrono::steady_clock::time_point last_escape_;

    auto conversation_view = Renderer([&] {
        // 单线程消费 WS 事件：先吞队列，再构建视图
        bool had = state_->drain_events();
        // 合并刷新后复位 Post 标志；若消费期间又有新事件到达，重新触发一次
        notify_pending = false;
        if (had && !state_->queue_empty())
            screen.Post(Event::Custom);

        int vw = std::max(10, Terminal::Size().dimx);
        conv_layout_ = render_conversation(*state_, vw - 2, hover_row_);
        Element content = conv_layout_.content;

        // 行级滚动 → focusPositionRelative 比例
        int total_rows = (int)conv_layout_.row_owners.size();
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
        if (help_.visible || sessions_.visible || cmd_palette_visible_) return -1;
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
        int total = (int)conv_layout_.row_owners.size();
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
        int owner = conv_layout_.row_owners[content_row];
        if (owner < 0 || owner >= (int)state_->items.size()) return;
        auto& it = state_->items[owner];
        if (it.kind != ItemKind::ToolCall || !tool_foldable(it)) return;
        // 截断态：仅 "  more..." 行可点击展开；展开态：仅 ▾ 命令行可点击收回
        const std::string& sig = conv_layout_.row_sigs[content_row];
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

    auto main_renderer = Renderer(main_container, [&] {
        // 瞬时提示过期清理（定时线程轮询 notice_pending_ 触发渲染）
        if (notice_pending_.load()) {
            auto now = std::chrono::steady_clock::now();
            if (now - notice_at_ > std::chrono::seconds(3)) {
                notice_.clear();
                notice_pending_.store(false);
            }
        }

        if (state_->processing) spinner_frame_ = (spinner_frame_ + 1) % 10;

        ViewCtx vctx;
        vctx.term_w = Terminal::Size().dimx;
        vctx.term_h = Terminal::Size().dimy;
        vctx.state = state_.get();
        vctx.model = controller_.model();
        vctx.cwd = cwd_;
        vctx.notice = notice_;
        vctx.session_id = state_->current_session;
        vctx.connected = acp_.connected();
        vctx.yolo = controller_.yolo();
        vctx.processing = state_->processing;
        vctx.spinner_frame = spinner_frame_;

        auto status_bar = render_status_bar(vctx);

        // Header: 居中 Codis + 分隔线 + 内容 + 状态栏
        Elements header = {
            text(" Codis ") | bold | center,
            separator(),
            main_container->Render() | flex,
            status_bar,
        };
        auto body = vbox(std::move(header));

        // Confirm overlay — Ask 权限工具执行确认（优先级最高：压过 help/sessions）
        if (state_->pending_confirm) {
            auto& pc = *state_->pending_confirm;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - pc.received_at).count();
            int remain = (int)(pc.timeout_seconds - elapsed);
            if (remain < 0) remain = 0;
            return confirm_.render(body, pc.call, remain, vctx.term_w);
        }
        if (help_.visible) return help_.render(body, kCommands);
        if (info_.visible) return info_.render(body);
        if (sessions_.visible && !sessions_.list.empty())
            return sessions_.render(body, state_->current_session);
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

        // 工具确认模态（Ask 权限）：锁定界面，仅接受确认相关输入
        if (state_->pending_confirm) {
            confirm_.handle_key(event);
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
                        controller_.cancel_task();
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

        if (help_.handle_key(event)) return true;
        if (info_.handle_key(event)) return true;
        if (sessions_.handle_key(event)) return true;

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
            controller_.open_sessions();
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
            // 确认框打开时持续重绘：倒计时每秒跳动 + 超时临界变色
            if (notice_pending_.load() || state_->processing ||
                state_->pending_confirm.has_value())
                screen.Post(Event::Custom);
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

void TuiClient::respond_confirm(bool approve) {
    auto& pc = *state_->pending_confirm;
    acp_.send_confirmation(pc.confirm_id, approve);
    show_notice(approve ? "[Tool approved]" : "[Tool rejected]");
    state_->pending_confirm.reset();
    post_job_();
}

} // namespace codis