#pragma once

#include "tui.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <string>
#include <utility>
#include <vector>

namespace codis {

using namespace ftxui;

// =============================================================================
// 换行工具 — UTF-8 感知、按显示宽度换行（CJK 等宽字符按 2 列计）
// =============================================================================

inline std::vector<std::string> wrap_by_width(const std::string& text, int width) {
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
inline Element wrapped_text(const std::string& content, int width) {
    Elements els;
    for (auto& ln : wrap_by_width(content, width)) els.push_back(ftxui::text(ln));
    return vbox(std::move(els));
}

// =============================================================================
// 逐行渲染单元 — 元素 + 文本签名（命中测试/行数/渲染三者同源）
// sig = 该行渲染后的可见字符序列（不含行尾空白），用于点击坐标反查。
// =============================================================================

struct UiRow {
    Element el;
    std::string sig;
};

// 纯文本行（可选颜色）
inline std::vector<UiRow> wrap_rows(const std::string& src, int width, Color c = Color::Default) {
    std::vector<UiRow> rows;
    for (auto& wl : wrap_by_width(src, width)) {
        auto el = text(wl);
        if (c != Color::Default) el = el | color(c);
        rows.push_back({std::move(el), wl});
    }
    return rows;
}

// 样式分段文本 → 按宽度换行成逐行元素; 行内样式按字节边界切分
// （行尾空格丢弃逻辑与 wrap_by_width 一致）
struct MdSeg {
    std::string text;
    Color fg = Color::Default;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool code = false;   // 行内代码: 纸片底色（与代码块同色系）
    bool strike = false;
};

inline int md_utf8_len(char c) {
    unsigned char u = (unsigned char)c;
    if (u >= 0xF0) return 4;
    if (u >= 0xE0) return 3;
    if (u >= 0xC0) return 2;
    return 1;
}

// 样式分段 → 拼接文本（可选每段字节边界）
inline std::string md_text_of(const std::vector<MdSeg>& segs, std::vector<size_t>* bounds) {
    std::string out;
    if (bounds) {
        bounds->clear();
        bounds->push_back(0);
    }
    for (auto& s : segs) {
        out += s.text;
        if (bounds) bounds->push_back(out.size());
    }
    return out;
}

inline Element md_seg_el(const MdSeg& s) {
    auto el = text(s.text);
    if (s.bold) el = el | bold;
    if (s.dim) el = el | dim;
    if (s.italic) el = el | italic;
    if (s.strike) el = el | strikethrough;
    if (s.code) el = el | bgcolor(Color(Color::Palette256::Grey19));
    if (s.fg != Color::Default) el = el | color(s.fg);
    return el;
}

// 样式分段 → 逐行渲染; 每行一条 hbox<分段>（sig = 该行可见文本）
inline std::vector<UiRow> md_styled_rows(const std::vector<MdSeg>& segs, int width) {
    std::vector<size_t> bounds;
    std::string text = md_text_of(segs, &bounds);
    std::vector<UiRow> rows;
    size_t pos = 0;  // 原串已消费字节
    for (auto& ln : wrap_by_width(text, width)) {
        size_t start = pos;
        size_t k = 0;
        while (k < ln.size()) {
            int cl = md_utf8_len(ln[k]);
            k += (size_t)cl;
            pos += (size_t)cl;
        }
        while (pos < text.size() && text[pos] == ' ') pos++;  // 行尾被丢弃的空格
        size_t end = pos;
        Elements parts;
        size_t si = 0;
        while (si + 1 < bounds.size() && bounds[si + 1] <= start) si++;
        size_t cursor = start;
        while (cursor < end && si + 1 < bounds.size()) {
            size_t s_end = std::min(bounds[si + 1], end);
            if (s_end > cursor) {
                MdSeg seg = segs[si];
                seg.text = ln.substr(cursor - start, s_end - cursor);
                parts.push_back(md_seg_el(seg));
            }
            cursor = s_end;
            if (cursor >= bounds[si + 1]) si++;
        }
        std::string sig = ln;
        while (!sig.empty() && sig.back() == ' ') sig.pop_back();
        rows.push_back({hbox(std::move(parts)), sig});
    }
    return rows;
}

// =============================================================================
// openCode 风格的工具调用渲染（ToolCall 条目 → 逐行元素）
// 命令行在块外（命令名绿色），结果/错误在块内；成功结果块可折叠。
// =============================================================================

// 完成态颜色（暗灰，低调）
inline const Color kToolMuted = Color::GrayLight;
inline const Color kToolError = Color::RedLight;
inline const Color kToolAdded = Color::GreenLight;
inline const Color kToolRemoved = Color::RedLight;

// 工具块逐行版本（每行 │ 左边框 + 面板底色，对应 openCode BlockTool）
inline std::vector<UiRow> block_wrap(const std::string& src, int width, Color c) {
    std::vector<UiRow> rows;
    int w = std::max(1, width - 2);
    for (auto& wl : wrap_by_width(src, w))
        rows.push_back({hbox({text("│ "), text(wl) | color(c) | flex}) |
                            bgcolor(Color(Color::Palette256::Grey19)),
                        "│ " + wl});
    return rows;
}

// diff 行 → 块行（按宽度换行 + 逐行 diff 着色）
inline std::vector<UiRow> diff_block_rows(const std::string& ln, int width) {
    std::vector<UiRow> rows;
    int w = std::max(1, width - 2);
    for (auto& wl : wrap_by_width(ln, w)) {
        auto el = text(wl);
        switch (classify_diff_line(wl)) {
            case DiffLineKind::Added:   el = el | color(kToolAdded); break;
            case DiffLineKind::Removed: el = el | color(kToolRemoved); break;
            default:                    el = el | color(kToolMuted); break;
        }
        rows.push_back({hbox({text("│ "), std::move(el) | flex}) |
                            bgcolor(Color(Color::Palette256::Grey19)),
                        "│ " + wl});
    }
    return rows;
}

inline Element tool_block(Elements lines) {
    Elements wrapped;
    for (auto& el : lines) wrapped.push_back(hbox({text("│ "), std::move(el) | flex}) |
                                            bgcolor(Color(Color::Palette256::Grey19)));
    return vbox(std::move(wrapped));
}

// 单行工具行（对应 openCode InlineTool）：[icon 宽2] label
inline Element tool_inline(const std::string& icon, const std::string& label, Color fg,
                           int width) {
    return hbox({text(icon) | size(WIDTH, EQUAL, 2),
                 wrapped_text(label, width - 2) | flex}) | color(fg);
}

// 命令名 = 首个空白前的词
inline size_t command_name_len(const std::string& s) {
    auto sp = s.find_first_of(" \t");
    return sp == std::string::npos ? s.size() : sp;
}

// 命令行两段着色：前缀 [0, green_bytes) 绿色（命令名），其余保持 rest_c（参数/符号）；
// 可选折叠标记（▸/▾，首行前，dim）。与 wrap_by_width 同流换行。
inline std::vector<UiRow> tool_command_split(const std::string& line_text, size_t green_bytes,
                                             int width, Color rest_c,
                                             const std::string& marker = "") {
    std::vector<MdSeg> segs;
    if (!marker.empty()) {
        MdSeg m;
        m.text = marker;
        m.dim = true;
        segs.push_back(std::move(m));
    }
    if (green_bytes > 0) {
        MdSeg g;
        g.text = line_text.substr(0, green_bytes);
        g.fg = Color::Green;
        segs.push_back(std::move(g));
    }
    MdSeg r;
    r.text = green_bytes < line_text.size() ? line_text.substr(green_bytes) : "";
    if (!r.text.empty()) {
        r.fg = rest_c;
        segs.push_back(std::move(r));
    }
    auto rows = md_styled_rows(segs, width);
    return rows;
}

// 工具结果内容的行块（块内宽度 width-2，与 block_wrap 一致）：
//   bash     → result_text
//   write/edit → content_text（去掉 "edit/write <path>" 首行）
//   通用工具 → result_text
inline std::vector<std::string> tool_content_lines(const ConvItem& item) {
    if (item.tool_name == "bash") return split_diff_lines(item.result_text);
    if (item.tool_name == "write" || item.tool_name == "edit") {
        auto lines = split_diff_lines(item.content_text);
        if (!lines.empty()) lines.erase(lines.begin());  // "edit/write <path>"
        return lines;
    }
    return split_diff_lines(item.result_text);
}

// 折叠时标记显示的结果行数（按渲染同宽 wrap 计数）
inline int tool_block_rows_count(const ConvItem& item, int width) {
    int n = 0;
    int w = std::max(1, width - 2);
    for (auto& ln : tool_content_lines(item)) n += (int)wrap_by_width(ln, w).size();
    return n;
}

// 命令显示行 + 绿色字节数：
//   bash     → "$ <command>"（$+命令名绿色）
//   write/edit → block_title（"# Wrote <path>" / "← Edit <path>"，标题随失败态着色）
//   通用工具 → "# <label>"（#+工具名绿色）
inline std::string tool_cmd_line(const ConvItem& item, size_t* green_bytes) {
    if (item.tool_name == "bash") {
        std::string s = "$ " + item.text;
        if (green_bytes) *green_bytes = 2 + command_name_len(item.text);
        return s;
    }
    if (item.tool_name == "write" || item.tool_name == "edit") {
        if (green_bytes) *green_bytes = 0;
        return !item.tool_title.empty() ? item.tool_title : "# " + item.text;
    }
    if (green_bytes) *green_bytes = 2 + command_name_len(item.text);
    return "# " + item.text;
}

inline Color tool_cmd_color(const ConvItem& item, bool failed) {
    if (failed) return kToolError;
    if (item.tool_name == "bash") return Color::Default;
    if (item.tool_name == "write" || item.tool_name == "edit") return kToolMuted;
    return kToolMuted;
}

// ToolCall 条目 → 逐行元素（含折叠）。结论由调用方（conversation renderer）统一使用。
inline std::vector<UiRow> render_tool_call(const ConvItem& item, int width) {
    const bool pending = !item.has_result;
    const bool failed = item.has_result && !item.tool_success;
    const bool inline_only =
        item.tool_name == "read" || item.tool_name == "glob" || item.tool_name == "grep";
    // 成功后且有结果块（bash/通用有输出、write/edit 恒有）才可折叠
    const bool foldable = item.has_result && !failed && !inline_only &&
                          !tool_content_lines(item).empty();

    if (pending)
        return {{text("~ " + item.tool_pending), "~ " + item.tool_pending}};

    // 失败：恒展开；命令行（红）在外，失败信息在块内
    if (failed) {
        if (inline_only) return {{tool_inline(item.tool_icon, item.text, kToolError, width),
                                  "→ " + item.text}};
        size_t gb = 0;
        std::string cmd = tool_cmd_line(item, &gb);
        auto rows = tool_command_split(cmd, gb, width, kToolError);
        std::vector<UiRow> inner;
        auto content = split_diff_lines(item.content_text);
        if (!content.empty()) content.erase(content.begin());  // "edit/write <path>"
        for (auto& ln : content) {
            auto dw = diff_block_rows(ln, width);
            for (auto& r : dw) inner.push_back(std::move(r));
        }
        for (auto& ln : split_diff_lines(item.error_text)) {
            auto bw = block_wrap(ln, width, kToolError);
            for (auto& r : bw) inner.push_back(std::move(r));
        }
        if (inner.empty()) return rows;
        rows.push_back({text(""), ""});
        for (auto& r : inner) rows.push_back(std::move(r));
        return rows;
    }

    // 折叠：只显示命令行 + 标记 + 行数
    if (foldable && item.folded) {
        int n = tool_block_rows_count(item, width);
        size_t gb = 0;
        std::string cmd = tool_cmd_line(item, &gb);
        cmd += "  (" + std::to_string(n) + (n == 1 ? " line" : " lines") + ")";
        return tool_command_split(cmd, gb, width, tool_cmd_color(item, false), "▸ ");
    }

    // 展开：命令行（▾ 标记 + 绿色命令名） + 空行 + 结果块
    if (foldable) {
        size_t gb = 0;
        std::string cmd = tool_cmd_line(item, &gb);
        auto rows = tool_command_split(cmd, gb, width, tool_cmd_color(item, false), "▾ ");
        rows.push_back({text(""), ""});
        Color bc = kToolMuted;
        if (item.tool_name == "bash") bc = kToolMuted;
        if (item.tool_name == "write" || item.tool_name == "edit") {
            for (auto& ln : tool_content_lines(item)) {
                auto dw = diff_block_rows(ln, width);
                for (auto& r : dw) rows.push_back(std::move(r));
            }
        } else {
            for (auto& ln : tool_content_lines(item)) {
                auto bw = block_wrap(ln, width, bc);
                for (auto& r : bw) rows.push_back(std::move(r));
            }
        }
        return rows;
    }

    // 无可折叠块的已完成工具
    if (item.tool_name == "write" || item.tool_name == "edit") {
        size_t gb = 0;
        std::string cmd = tool_cmd_line(item, &gb);
        auto rows = tool_command_split(cmd, gb, width, tool_cmd_color(item, false));
        rows.push_back({text(""), ""});
        for (auto& ln : tool_content_lines(item)) {
            auto dw = diff_block_rows(ln, width);
            for (auto& r : dw) rows.push_back(std::move(r));
        }
        return rows;
    }

    if (item.tool_name == "bash") {
        if (item.result_text.empty())
            return {{tool_inline(item.tool_icon, item.text, kToolMuted, width),
                     "$ " + item.text}};
        size_t gb = 0;
        std::string cmd = tool_cmd_line(item, &gb);
        auto rows = tool_command_split(cmd, gb, width, Color::Default);
        rows.push_back({text(""), ""});
        for (auto& ln : tool_content_lines(item)) {
            auto bw = block_wrap(ln, width, kToolMuted);
            for (auto& r : bw) rows.push_back(std::move(r));
        }
        return rows;
    }

    // 通用工具
    if (!item.result_text.empty()) {
        size_t gb = 0;
        std::string cmd = tool_cmd_line(item, &gb);
        auto rows = tool_command_split(cmd, gb, width, kToolMuted);
        rows.push_back({text(""), ""});
        for (auto& ln : tool_content_lines(item)) {
            auto bw = block_wrap(ln, width, kToolMuted);
            for (auto& r : bw) rows.push_back(std::move(r));
        }
        return rows;
    }

    return {{tool_inline(item.tool_icon, item.text, kToolMuted, width), "# " + item.text}};
}

} // namespace codis