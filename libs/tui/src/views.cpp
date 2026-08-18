#include "views.h"
#include "md_render.h"
#include "tui_tool_render.h"
#include "tool_format.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <sstream>

namespace codis {

using namespace ftxui;

// Sessions 面板的最大可见行数（固定面板高度，避免会话太多时撑满屏幕）
static constexpr int kMaxSessionRows = 12;

// 工具确认对话框（Ask 权限）：卡片宽度上限 / 参数区行数上限
static constexpr int kConfirmW = 72;
static constexpr int kConfirmArgLines = 9;

// =============================================================================
// 对话区
// =============================================================================

ConversationLayout render_conversation(const TuiState& st, int tw, int hover_row) {
    int vw = tw + 2;  // 预留 vscroll_indicator 一列 + 1 列余量
    Elements els;
    auto card_bg = bgcolor(Color(Color::Palette256::Grey7));

    ConversationLayout out;
    auto push_row = [&](Element el, std::string sig, int owner) {
        els.push_back(std::move(el));
        out.row_sigs.push_back(std::move(sig));
        out.row_owners.push_back(owner);
    };
    auto card_row = [&](Color bar_color, Element body, std::string body_sig, int owner) {
        push_row(hbox({text("┃ ") | color(bar_color) | bold, std::move(body) | flex}) | card_bg,
                 "┃ " + std::move(body_sig), owner);
    };

    {
        bool first = true;
        for (int oi = 0; oi < (int)st.items.size(); oi++) {
            auto& item = st.items[oi];
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
            case ItemKind::Reasoning: {
                // 独立思维链块：标签行 + 灰斜体内容，不用与消息同款的 "┃" 竖线卡片
                std::string rt = item.text;
                // 剥掉模型自带的 "thinking" 首行标记（GLM 思考模式 reasoning_content 以之开头）
                bool mark = (rt.rfind("thinking", 0) == 0 || rt.rfind("Thinking", 0) == 0) &&
                            (rt.size() == 8 || std::isspace((unsigned char)rt[8]));
                if (mark) {
                    size_t j = 8;
                    while (j < rt.size() && std::isspace((unsigned char)rt[j])) j++;
                    rt = rt.substr(std::min(j, rt.size()));
                }
                push_row(hbox({text(" 💭 thinking ") | italic | color(Color::GrayDark),
                               flex(text(""))}), "💭 thinking", oi);
                for (auto& r : wrap_rows(rt, tw - 3, Color::GrayDark))
                    push_row(hbox({text("   "), std::move(r.el) | italic}) | card_bg,
                             std::move(r.sig), oi);
                break;
            }
            case ItemKind::ToolCall:
                for (auto& r : render_tool_call(item, tw))
                    push_row(std::move(r.el), std::move(r.sig), oi);
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
            }
        }
    }

    // 悬停的折叠目标行高亮（下划线），点击前即可确认可点
    if (hover_row >= 0 && hover_row < (int)els.size()) {
        int owner = out.row_owners[hover_row];
        if (owner >= 0 && owner < (int)st.items.size()) {
            auto& it = st.items[owner];
            if (tool_row_is_fold_target(it, out.row_sigs[hover_row]))
                els[hover_row] = els[hover_row] | underlined;
        }
    }

    out.content = vbox(std::move(els)) | size(WIDTH, LESS_THAN, vw);
    return out;
}

// =============================================================================
// 状态栏 / 命令补全弹窗
// =============================================================================

Element render_status_bar(const ViewCtx& ctx) {
    Elements status_lines;
    status_lines.push_back(hbox({
        text(ctx.connected ? " ● Connected" : " ● Disconnected") |
            (ctx.connected ? color(Color::Green) : color(Color::Red)),
        text("  │  " + ctx.model) | dim,
        text("  │  context " + (ctx.state ? ctx.state->context_size_str() : "?")) | dim,
        ctx.yolo ? text("  │  ⚡YOLO ") | color(Color::Yellow) | bold : text(""),
        flex(text("")),
        text("  " + ctx.session_id.substr(0, 8)) | dim | inverted,
    }) | bgcolor(Color(Color::Palette256::Grey7)));
    static constexpr const char* kSpinner[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    static constexpr int kSpinnerCount = 10;
    status_lines.push_back(hbox({
        text(ctx.processing ? (std::string(" ") + kSpinner[ctx.spinner_frame % kSpinnerCount] +
                               " processing...")
                            : std::string(" ● idle")) |
            (ctx.processing ? color(Color::Yellow) : color(Color::Green)),
        flex(text("")),
        ctx.notice.empty() ? text("") : text("  " + ctx.notice + " ") | color(Color::Yellow),
    }) | bgcolor(Color(Color::Palette256::Grey7)));
    status_lines.push_back(text("") | bgcolor(Color(Color::Palette256::Grey7)));
    status_lines.push_back(hbox({
        text(" " + ctx.cwd) | dim,
        flex(text("")),
    }) | bgcolor(Color(Color::Palette256::Grey7)));
    status_lines.push_back(text("") | bgcolor(Color(Color::Palette256::Grey7)));
    return vbox(std::move(status_lines));
}

Element render_cmd_palette(const std::vector<std::pair<std::string, std::string>>& filtered,
                           int selected) {
    if (filtered.empty()) return text("");
    Elements rows;
    for (int i = 0; i < (int)filtered.size(); i++) {
        auto& [cmd, desc] = filtered[i];
        auto el = text("  " + cmd) | flex;
        if (i == selected) el = el | inverted;
        rows.push_back(hbox({el, text("  " + desc) | dim}));
    }
    return window(text(" Commands "), vbox(std::move(rows)) | frame) | clear_under | border;
}

// =============================================================================
// HelpOverlay
// =============================================================================

bool HelpOverlay::handle_key(Event e) {
    if (!visible) return false;
    if (e == Event::Escape) {
        visible = false;
        return true;
    }
    return true;  // 面板打开时吞掉其它按键（与 Sessions 一致）
}

Element HelpOverlay::render(Element body,
                            const std::vector<std::pair<std::string, std::string>>& commands) const {
    if (!visible) return body;
    Elements cmd_rows;
    for (const auto& [cmd, desc] : commands)
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
    return dbox({std::move(body), overlay});
}

// =============================================================================
// InfoOverlay（/info — skills & MCP 状态）
// =============================================================================

bool InfoOverlay::handle_key(Event e) {
    if (!visible) return false;
    if (e == Event::Tab || e == Event::TabReverse) {
        pane = 1 - pane;
        return true;
    }
    int n = (pane == 0) ? (int)skills.size() : (int)mcps.size();
    if (e == Event::ArrowUp && sel[pane] > 0) {
        sel[pane]--;
        return true;
    }
    if (e == Event::ArrowDown && sel[pane] < n - 1) {
        sel[pane]++;
        return true;
    }
    if (e == Event::Escape) {
        visible = false;
        return true;
    }
    return true;  // 面板打开时吞掉其它按键（与 Sessions 一致）
}

Element InfoOverlay::render(Element body) const {
    if (!visible) return body;
    auto trunc = [](const std::string& s, size_t n) {
        if (s.size() <= n) return s;
        return s.substr(0, n) + "…";
    };
    auto skill_rows = [&]() {
        Elements rows;
        if (skills.empty()) {
            rows.push_back(text("  (none)") | dim);
        } else {
            for (int i = 0; i < (int)skills.size(); i++) {
                auto& s = skills[i];
                bool cur = (pane == 0 && i == sel[0]);
                auto name_el = text((cur ? "▸ " : "  ") + s.id);
                if (cur) name_el = name_el | inverted;
                rows.push_back(name_el);
                rows.push_back(text("    " +
                    trunc(s.description.empty() ? s.name : s.description, 34)) | dim);
            }
        }
        return vbox(std::move(rows)) | frame | vscroll_indicator;
    };
    auto mcp_rows = [&]() {
        Elements rows;
        if (mcps.empty()) {
            rows.push_back(text("  (none)") | dim);
        } else {
            for (int i = 0; i < (int)mcps.size(); i++) {
                auto& m = mcps[i];
                bool cur = (pane == 1 && i == sel[1]);
                auto name_el = text(std::string(cur ? "▸ " : "  ") +
                    (m.online ? "● " : "○ ") + m.name) |
                    (m.online ? color(Color::Green) : color(Color::Red));
                if (cur) name_el = name_el | inverted;
                rows.push_back(name_el);
                rows.push_back(text("    " + m.transport + " · " +
                    std::to_string(m.tool_count) + " tools") | dim);
            }
        }
        return vbox(std::move(rows)) | frame | vscroll_indicator;
    };

    auto overlay = window(text(" Skills & MCP "), vbox({
        text(" " + std::to_string(skills.size()) + " skills · " +
             std::to_string(mcps.size()) + " MCP servers") | bold,
        separator(),
        hbox({
            vbox({text(" Skills") | bold, skill_rows()}) | flex,
            text("  │  ") | dim,
            vbox({text(" MCP Servers") | bold, mcp_rows()}) | flex,
        }),
        separator(),
        text(" ↑↓ 移动 · Tab 切栏 · ESC 关闭 ") | dim | center,
    })) | clear_under | center | border |
        size(WIDTH, LESS_THAN, 92) | size(HEIGHT, LESS_THAN, 28);
    return dbox({std::move(body), overlay});
}

// =============================================================================
// SessionsOverlay
// =============================================================================

bool SessionsOverlay::handle_key(Event e) {
    if (!visible) return false;
    if (e == Event::Tab) {
        selected = (selected - 1 + (int)list.size()) % (int)list.size();
        return true;
    }
    if (e == Event::TabReverse) {
        selected = (selected + 1) % (int)list.size();
        return true;
    }
    if (e == Event::ArrowUp && selected > 0) {
        selected--;
        return true;
    }
    if (e == Event::ArrowDown && selected < (int)list.size() - 1) {
        selected++;
        return true;
    }
    if ((e == Event::d || e == Event::D) && !list.empty()) {
        if (on_delete) on_delete(list[selected]);
        return true;
    }
    if (e == Event::Return && !list.empty()) {
        if (on_activate) on_activate(list[selected]);
        return true;
    }
    if (e == Event::Escape) {
        visible = false;
        return true;
    }
    return true;  // 面板打开时吞掉其它按键
}

Element SessionsOverlay::render(Element body, const std::string& current_session) const {
    if (!visible || list.empty()) return body;
    Elements rows;
    for (int i = 0; i < (int)list.size(); i++) {
        auto& s = list[i];
        std::string prefix = (s.id == current_session) ? "> " : "  ";
        auto el = text(prefix + s.id + "  " + std::to_string(s.message_count) + " msgs  " + s.title);
        if (s.id == current_session) el = el | bold;
        if (i == selected) el = el | inverted | focus;
        rows.push_back(el);
    }

    auto overlay = window(text(" Sessions "), vbox({
        vbox(std::move(rows)) | frame | size(HEIGHT, EQUAL, kMaxSessionRows) |
            vscroll_indicator,
        separator(),
        text(" " + std::to_string(selected + 1) + "/" +
             std::to_string(list.size()) + "  ↑↓/Tab  Enter(del)  ESC ") | dim | center,
    })) | clear_under | center | border;

    return dbox({std::move(body), overlay});
}

// =============================================================================
// ModelOverlay（/model — 模型下拉选择，Tab/↑↓ 循环，Enter 应用）
// =============================================================================

bool ModelOverlay::handle_key(Event e) {
    if (!visible) return false;
    if (list.empty()) {
        if (e == Event::Escape) {
            visible = false;
            return true;
        }
        return true;  // 面板打开时吞掉其它按键
    }
    if (e == Event::Tab || e == Event::ArrowDown) {
        selected = (selected + 1) % (int)list.size();
        return true;
    }
    if (e == Event::TabReverse || e == Event::ArrowUp) {
        selected = (selected - 1 + (int)list.size()) % (int)list.size();
        return true;
    }
    if (e == Event::Return) {
        if (on_activate) on_activate(list[selected].first);
        return true;
    }
    if (e == Event::Escape) {
        visible = false;
        return true;
    }
    return true;  // 面板打开时吞掉其它按键
}

Element ModelOverlay::render(Element body, const std::string& current_provider) const {
    if (!visible || list.empty()) return body;
    Elements rows;
    for (int i = 0; i < (int)list.size(); i++) {
        auto& [provider, model] = list[i];
        bool current = (provider == current_provider);
        auto el = text((current ? "> " : "  ") + provider +
                       (model.empty() ? "" : "  (" + model + ")"));
        if (current) el = el | bold;
        if (i == selected) el = el | inverted | focus;
        rows.push_back(el);
    }

    auto overlay = window(text(" Models "), vbox({
        vbox(std::move(rows)) | frame | size(HEIGHT, EQUAL, kMaxSessionRows) |
            vscroll_indicator,
        separator(),
        text(" " + std::to_string(selected + 1) + "/" +
             std::to_string(list.size()) + "  Tab/↑↓  Enter 应用  ESC ") | dim | center,
    })) | clear_under | center | border;

    return dbox({std::move(body), overlay});
}

// =============================================================================
// ConfirmOverlay（Ask 权限工具执行确认）
// =============================================================================

bool ConfirmOverlay::handle_key(Event e) {
    if (e == Event::Tab || e == Event::TabReverse ||
        e == Event::ArrowLeft || e == Event::ArrowRight) {
        focus = !focus;
        return true;
    }
    if (e == Event::Character('y') || e == Event::Character('Y')) {
        if (on_respond) on_respond(true);
        return true;
    }
    if (e == Event::Character('n') || e == Event::Character('N') ||
        e == Event::Escape) {
        if (on_respond) on_respond(false);
        return true;
    }
    if (e == Event::Return) {
        if (on_respond) on_respond(focus);  // Enter 激活焦点按钮（默认焦点=拒绝，安全默认）
        return true;
    }
    if (e.is_mouse()) {
        // 左键释放且命中按钮行：批准(左半) / 拒绝(右半)；其余鼠标输入模态吞掉
        if (e.mouse().button == Mouse::Left &&
            e.mouse().motion == Mouse::Released &&
            height > 0) {
            int term_w = Terminal::Size().dimx;
            int cw = std::min(kConfirmW, std::max(40, term_w - 8));
            int cx = (term_w - (cw + 2)) / 2;
            int top = (Terminal::Size().dimy - height) / 2;
            int btn_y = top + height - 3;  // 内容倒数第 2 行（按钮行）
            if (e.mouse().y == btn_y &&
                e.mouse().x >= cx && e.mouse().x < cx + cw + 2) {
                if (on_respond) on_respond(e.mouse().x < cx + (cw + 2) / 2);
            }
        }
        return true;
    }
    return true;  // 其它按键一律吞掉，不落入输入框
}

Element ConfirmOverlay::render(Element body, const acp::ToolCallEvent& call, int remain_secs,
                               int term_w) {
    const auto& args = call.arguments;

    int cw = std::min(kConfirmW, std::max(40, term_w - 8));  // 对话框宽
    int text_w = cw - 4;  // 参数行截断宽

    Elements arg_lines;
    auto push_line = [&](std::string s, Color c, bool b = false) {
        if ((int)s.size() > text_w) s = s.substr(0, text_w - 1) + "…";
        auto el = text(std::move(s)) | color(c);
        if (b) el = el | bold;
        arg_lines.push_back(std::move(el));
    };
    auto push_block = [&](const std::string& label, const std::string& s,
                          int max_lines, Color c) {
        if (s.empty()) { push_line(label + " (empty)", c); return; }
        std::istringstream iss(s);
        std::string l;
        int i = 0;
        bool truncated = false;
        for (; std::getline(iss, l); ++i) {
            if (i >= max_lines) { truncated = true; break; }
            push_line((i == 0 ? label : "") + (i == 0
                ? "" : std::string(std::min(text_w, (int)label.size()), ' ')) + l, c);
        }
        if (truncated) push_line(("… +" + std::to_string(i) + " more lines"), Color::GrayDark);
    };

    // 按工具类型渲染参数区
    if (call.name == "bash") {
        std::string cmd = args.value("command", "");
        if (cmd.empty()) push_line("$ (empty command)", Color::GrayDark);
        else push_line("$ " + cmd, Color::Cyan, true);
    } else if (call.name == "write") {
        push_line("path: " + args.value("filePath", ""), Color::Yellow);
        push_block("content: ", args.value("content", ""), kConfirmArgLines - 1, Color::Green);
    } else if (call.name == "edit") {
        push_line("path: " + args.value("filePath", ""), Color::Yellow);
        push_block("old: ", args.value("oldString", ""), 4, Color::Red);
        push_block("new: ", args.value("newString", ""), 4, Color::Green);
    } else {
        std::string dump = args.dump();
        if ((int)dump.size() > text_w * (kConfirmArgLines + 1)) dump.resize(text_w * (kConfirmArgLines + 1));
        push_block("args: ", dump, kConfirmArgLines, Color::White);
    }

    int n_args = (int)arg_lines.size();
    height = n_args + 8;  // 内容 n_args+6 + 边框 2

    auto approve_el = text(focus ? " [ ✓ 批准 (y) ] " : " [ 批准 ] ");
    approve_el = focus ? (approve_el | inverted) | bold : (approve_el | dim);
    auto reject_el = text(!focus ? " [ ✗ 拒绝 (n) ] " : " [ 拒绝 ] ");
    reject_el = !focus ? (reject_el | inverted) | bold : (reject_el | dim);

    bool danger = call.name == "bash";
    auto overlay = window(
        text(danger ? " ⚠ 执行确认 — bash " : " ⚠ 工具执行确认 ") | bold |
            color(danger ? Color::Red : Color::Yellow),
        vbox({
            hbox({ text(" 工具 "), text(call.name) | bold |
                       color(danger ? Color::Red : Color::Yellow),
                   flex(text("")) }),
            separator(),
            vbox(std::move(arg_lines)) | frame,
            separator(),
            text(" ⏳ " + std::to_string(remain_secs) +
                 (remain_secs <= 10 ? "s 即将超时，未确认将自动拒绝 " : "s 内未确认将自动拒绝 ")) |
                (remain_secs <= 10 ? color(Color::Red) : dim),
            hbox({ flex(text("")), approve_el, text("   "), reject_el,
                   flex(text("")) }),
            text(" Tab/←→ 切换 · y/Enter 批准 · n/Esc 拒绝 ") | dim | center,
        }) | size(WIDTH, LESS_THAN, cw) | size(HEIGHT, LESS_THAN, n_args + 6))
        | clear_under | center | border;
    return dbox({std::move(body), overlay});
}

} // namespace codis
