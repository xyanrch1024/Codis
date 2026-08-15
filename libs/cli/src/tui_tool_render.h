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

// 命令行（块外）：普通行，无左边框、无面板底色
inline Element tool_command(const std::string& cmd, int width, Color c) {
    return vbox(tool_wrapped(cmd, width, c));
}

// 命令名 = 首个空白前的词
inline size_t command_name_len(const std::string& s) {
    auto sp = s.find_first_of(" \t");
    return sp == std::string::npos ? s.size() : sp;
}

// 命令行两段着色：前缀 [0, green_bytes) 绿色（命令名），其余保持 rest_c（参数/符号）
// 与 wrap_by_width 同流换行，逐行推算绿色段与参数段的字节边界
inline Element tool_command_split(const std::string& line_text, size_t green_bytes,
                                  int width, Color rest_c) {
    auto utf8_len = [](char c) {
        unsigned char u = (unsigned char)c;
        if (u >= 0xF0) return 4;
        if (u >= 0xE0) return 3;
        if (u >= 0xC0) return 2;
        return 1;
    };
    Elements rows;
    size_t pos = 0;  // 原串已消费字节
    for (auto& ln : wrap_by_width(line_text, width)) {
        size_t start = pos;
        size_t k = 0;
        while (k < ln.size()) {
            int cl = utf8_len(ln[k]);
            k += static_cast<size_t>(cl);
            pos += static_cast<size_t>(cl);
        }
        while (pos < line_text.size() && line_text[pos] == ' ') pos++;  // 行尾被丢弃的空格
        size_t end = pos;
        Elements segs;
        if (green_bytes > 0 && start < green_bytes) {
            size_t split = std::min(end, green_bytes);
            segs.push_back(text(ln.substr(0, split - start)) | color(Color::Green));
            if (split < end) segs.push_back(text(ln.substr(split - start)) | color(rest_c));
        } else {
            segs.push_back(text(ln) | color(rest_c));
        }
        rows.push_back(hbox(std::move(segs)));
    }
    return vbox(std::move(rows));
}

// 命令行 + 空行 + 结果块
inline Element tool_command_block(Element header, Element block) {
    return vbox({std::move(header), text(""), std::move(block)});
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
        // 命令行在块外（bash → "$ <command>"；write/edit → 保留 block_title；
        // 通用工具 → "# <label>"），失败信息在块内
        Element header;
        if (item.tool_name == "bash")
            header = tool_command_split("$ " + item.text, 2 + command_name_len(item.text),
                                        width, kToolError);
        else if (!item.tool_title.empty())
            header = tool_command(item.tool_title, width, kToolError);
        else
            header = tool_command_split("# " + item.text, 2 + command_name_len(item.text),
                                        width, kToolError);
        Elements lines;
        auto content = split_diff_lines(item.content_text);
        if (!content.empty()) content.erase(content.begin());  // 去掉 "edit/write <path>" 首行
        for (auto& ln : content) {
            auto wrapped = tool_diff_lines(ln, width);
            lines.insert(lines.end(), wrapped.begin(), wrapped.end());
        }
        for (auto& ln : split_diff_lines(item.error_text))
            for (auto& wl : tool_wrapped(ln, width, kToolError)) lines.push_back(wl);
        if (lines.empty()) return header;
        return tool_command_block(header, tool_block(std::move(lines)));
    }

    if (item.tool_name == "write" || item.tool_name == "edit") {
        // block_title（"# Wrote <path>" / "← Edit <path>"）在块外，diff 在块内
        Element header;
        if (!item.tool_title.empty())
            header = tool_command(item.tool_title, width, kToolMuted);
        else
            header = tool_command("# " + item.text, width, kToolMuted);
        Elements lines;
        auto content = split_diff_lines(item.content_text);
        if (!content.empty()) content.erase(content.begin());  // 去掉 "edit/write <path>" 首行
        for (auto& ln : content) {
            auto wrapped = tool_diff_lines(ln, width);
            lines.insert(lines.end(), wrapped.begin(), wrapped.end());
        }
        return tool_command_block(header, tool_block(std::move(lines)));
    }

    if (item.tool_name == "bash") {
        auto out = split_diff_lines(item.result_text);
        if (out.empty())
            return tool_inline(item.tool_icon, item.text, kToolMuted, width);
        // "$ <command>" 在块外：命令名绿色，参数原色；输出在块内
        Elements lines;
        for (auto& ln : out)
            for (auto& wl : tool_wrapped(ln, width, kToolMuted)) lines.push_back(wl);
        return tool_command_block(
            tool_command_split("$ " + item.text, 2 + command_name_len(item.text),
                               width, Color::Default),
            tool_block(std::move(lines)));
    }

    // 通用工具：有输出则转块展示（"# <label>" 在块外，结果在块内）
    if (!item.result_text.empty()) {
        Elements lines;
        for (auto& ln : split_diff_lines(item.result_text))
            for (auto& wl : tool_wrapped(ln, width, kToolMuted)) lines.push_back(wl);
        return tool_command_block(
            tool_command_split("# " + item.text, 2 + command_name_len(item.text),
                               width, kToolMuted),
            tool_block(std::move(lines)));
    }

    return tool_inline(item.tool_icon, item.text, kToolMuted, width);
}

} // namespace codis
