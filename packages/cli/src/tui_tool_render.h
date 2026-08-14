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

// 单条源行 → 若干已换行的单行 text 元素（同一颜色）
inline Elements tool_wrapped(const std::string& line, int width, Color c) {
    Elements out;
    for (auto& wl : wrap_by_width(line, width)) out.push_back(text(wl) | color(c));
    return out;
}

// =============================================================================
// openCode 风格的工具调用渲染（ToolCall 条目 → Element）
// =============================================================================

// 完成态颜色（暗灰，低调）
inline const Color kToolMuted = Color::GrayLight;
inline const Color kToolError = Color::RedLight;
inline const Color kToolAdded = Color::GreenLight;
inline const Color kToolRemoved = Color::RedLight;

// diff 行逐行着色（edit/write 块内容），返回换行后的 Elements
inline Elements tool_diff_lines(const std::string& line, int width) {
    switch (classify_diff_line(line)) {
        case DiffLineKind::Added:   return tool_wrapped(line, width, kToolAdded);
        case DiffLineKind::Removed: return tool_wrapped(line, width, kToolRemoved);
        default:                    return tool_wrapped(line, width, kToolMuted);
    }
}

// 工具块：左边框 + 面板底色（对应 openCode BlockTool）
// 每行元素都必须已是单行 text（调用方需先按宽度展开），保证 "│ " 贯通每行
inline Element tool_block(Elements lines) {
    Elements wrapped;
    for (auto& el : lines) wrapped.push_back(hbox({text("│ "), std::move(el) | flex}));
    return vbox(std::move(wrapped)) | bgcolor(Color(Color::Palette256::Grey19));
}

// 单行工具行（对应 openCode InlineTool）：[icon 宽2] label
inline Element tool_inline(const std::string& icon, const std::string& label, Color fg,
                           int width) {
    return hbox({text(icon) | size(WIDTH, EQUAL, 2),
                 wrapped_text(label, width - 2) | flex}) | color(fg);
}

inline Element render_tool_call(const ConvItem& item, int width) {
    const bool pending = !item.has_result;
    const bool failed = item.has_result && !item.tool_success;
    const bool inline_only =
        item.tool_name == "read" || item.tool_name == "glob" || item.tool_name == "grep";

    if (pending)
        return text("~ " + item.tool_pending);

    if (failed) {
        if (inline_only)
            return tool_inline(item.tool_icon, item.text, kToolError, width);
        Elements lines;
        // 失败块顶部显示被执行的命令：
        //   bash     → "$ <command>"
        //   write/edit → 保留 block_title（"# Wrote <path>" / "← Edit <path>"）
        //   通用工具 → "# <label>"
        if (item.tool_name == "bash") {
            for (auto& wl : tool_wrapped("$ " + item.text, width, kToolError)) lines.push_back(wl);
        } else if (!item.tool_title.empty()) {
            lines.push_back(text(item.tool_title) | color(kToolError));
        } else {
            for (auto& wl : tool_wrapped("# " + item.text, width, kToolError)) lines.push_back(wl);
        }
        auto content = split_diff_lines(item.content_text);
        if (!content.empty()) content.erase(content.begin());  // 去掉 "edit/write <path>" 首行
        for (auto& ln : content) {
            auto wrapped = tool_diff_lines(ln, width);
            lines.insert(lines.end(), wrapped.begin(), wrapped.end());
        }
        for (auto& ln : split_diff_lines(item.error_text))
            for (auto& wl : tool_wrapped(ln, width, kToolError)) lines.push_back(wl);
        return tool_block(std::move(lines));
    }

    if (item.tool_name == "write" || item.tool_name == "edit") {
        Elements lines;
        if (!item.tool_title.empty())
            lines.push_back(text(item.tool_title) | color(kToolMuted));
        auto content = split_diff_lines(item.content_text);
        if (!content.empty()) content.erase(content.begin());  // 去掉 "edit/write <path>" 首行
        for (auto& ln : content) {
            auto wrapped = tool_diff_lines(ln, width);
            lines.insert(lines.end(), wrapped.begin(), wrapped.end());
        }
        return tool_block(std::move(lines));
    }

    if (item.tool_name == "bash") {
        auto out = split_diff_lines(item.result_text);
        if (out.empty())
            return tool_inline(item.tool_icon, item.text, kToolMuted, width);
        Elements lines;
        for (auto& wl : tool_wrapped("$ " + item.text, width, Color::Default)) lines.push_back(wl);
        for (auto& ln : out)
            for (auto& wl : tool_wrapped(ln, width, kToolMuted)) lines.push_back(wl);
        return tool_block(std::move(lines));
    }

    // 通用工具：有输出则转块展示
    if (!item.result_text.empty()) {
        Elements lines;
        for (auto& wl : tool_wrapped("# " + item.text, width, kToolMuted)) lines.push_back(wl);
        for (auto& ln : split_diff_lines(item.result_text))
            for (auto& wl : tool_wrapped(ln, width, kToolMuted)) lines.push_back(wl);
        return tool_block(std::move(lines));
    }

    return tool_inline(item.tool_icon, item.text, kToolMuted, width);
}

} // namespace codis
