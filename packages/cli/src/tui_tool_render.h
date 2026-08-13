#pragma once

#include "tui.h"

#include <ftxui/dom/elements.hpp>
#include <string>
#include <utility>
#include <vector>

namespace opencode {

using namespace ftxui;

// =============================================================================
// openCode 风格的工具调用渲染（ToolCall 条目 → Element）
// =============================================================================

// 完成态颜色（暗灰，低调）
inline const Color kToolMuted = Color::GrayLight;
inline const Color kToolError = Color::RedLight;
inline const Color kToolAdded = Color::GreenLight;
inline const Color kToolRemoved = Color::RedLight;

// diff 行逐行着色（edit/write 块内容）
inline Element tool_diff_line(const std::string& line) {
    switch (classify_diff_line(line)) {
        case DiffLineKind::Added:   return paragraph(line) | color(kToolAdded);
        case DiffLineKind::Removed: return paragraph(line) | color(kToolRemoved);
        default:                    return paragraph(line) | color(kToolMuted);
    }
}

// 工具块：左边框 + 面板底色（对应 openCode BlockTool）
inline Element tool_block(Elements lines) {
    Elements wrapped;
    for (auto& el : lines) wrapped.push_back(hbox({text("│ "), std::move(el) | flex}));
    return vbox(std::move(wrapped)) | bgcolor(Color(Color::Palette256::Grey19));
}

// 单行工具行（对应 openCode InlineTool）：[icon 宽2] label
inline Element tool_inline(const std::string& icon, const std::string& label, Color fg) {
    return hbox({text(icon) | size(WIDTH, EQUAL, 2), paragraph(label) | flex}) | color(fg);
}

inline Element render_tool_call(const ConvItem& item) {
    const bool pending = !item.has_result;
    const bool failed = item.has_result && !item.tool_success;
    const bool inline_only =
        item.tool_name == "read" || item.tool_name == "glob" || item.tool_name == "grep";

    if (pending)
        return text("~ " + item.tool_pending);

    if (failed) {
        if (inline_only)
            return tool_inline(item.tool_icon, item.text, kToolError);
        Elements lines;
        if (!item.tool_title.empty())
            lines.push_back(paragraph(item.tool_title) | color(kToolError));
        auto content = split_diff_lines(item.content_text);
        if (!content.empty()) content.erase(content.begin());  // 去掉 "edit/write <path>" 首行
        for (auto& ln : content) lines.push_back(tool_diff_line(ln));
        if (!item.error_text.empty())
            for (auto& ln : split_diff_lines(truncate_tool_output(item.error_text)))
                lines.push_back(paragraph(ln) | color(kToolError));
        return tool_block(std::move(lines));
    }

    if (item.tool_name == "write" || item.tool_name == "edit") {
        Elements lines;
        lines.push_back(paragraph(item.tool_title) | color(kToolMuted));
        auto content = split_diff_lines(item.content_text);
        if (!content.empty()) content.erase(content.begin());  // 去掉 "edit/write <path>" 首行
        for (auto& ln : content) lines.push_back(tool_diff_line(ln));
        return tool_block(std::move(lines));
    }

    if (item.tool_name == "bash") {
        auto out = truncate_tool_output(item.result_text);
        if (out.empty())
            return tool_inline(item.tool_icon, item.text, kToolMuted);
        Elements lines;
        lines.push_back(paragraph("$ " + item.text));
        for (auto& ln : split_diff_lines(out)) lines.push_back(paragraph(ln) | color(kToolMuted));
        return tool_block(std::move(lines));
    }

    // 通用工具：有输出则转块展示
    if (!item.result_text.empty()) {
        Elements lines;
        lines.push_back(paragraph("# " + item.text) | color(kToolMuted));
        for (auto& ln : split_diff_lines(truncate_tool_output(item.result_text)))
            lines.push_back(paragraph(ln) | color(kToolMuted));
        return tool_block(std::move(lines));
    }

    return tool_inline(item.tool_icon, item.text, kToolMuted);
}

} // namespace opencode
