#pragma once

#include "tui_tool_render.h"

#include <string>
#include <vector>

namespace codis {

using namespace ftxui;

// =============================================================================
// 轻量 Markdown 渲染 — Assistant 消息专用（含流式增量与历史回放，同一路径）
// 块级: 标题 / 围栏代码 / 列表 / 引用 / 分隔线 / 段落
// 行内: `code` / **bold** / *italic* / ~~strike~~ / [label](url)
// 容错: 未闭合围栏按代码到文本末尾、未闭合行内标记按字面显示。
// md_rows() 输出逐行元素 + 签名（sig），行数 = sig 数，与渲染/命中测试同源。
// =============================================================================

enum class MdKind { Para, Heading, Code, Quote, List, HR, Blank };

struct MdBlock {
    MdKind kind;
    std::vector<MdSeg> segs;   // 一段逻辑行（无 \n）
    std::string lang;          // Code: 围栏语言
    int level = 0;             // Heading 级别
};

inline std::string md_trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    return s.substr(b, e - b);
}

// 分隔线: --- / *** / ___（≥3 个同字符，其余仅空白）
inline bool md_is_hr(const std::string& s) {
    if (s.size() < 3) return false;
    char c = s[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (size_t i = 1; i < s.size(); i++)
        if (s[i] != c) return false;
    return true;
}

// 段落行 → 样式分段（`code`, **bold**, *italic*, ~~strike~~, [label](url)）
inline std::vector<MdSeg> md_inline(const std::string& line) {
    std::vector<MdSeg> segs;
    MdSeg cur;
    auto flush = [&] {
        if (!cur.text.empty()) segs.push_back(std::move(cur));
        cur = MdSeg{};
    };
    size_t i = 0, n = line.size();
    while (i < n) {
        char c0 = line[i];
        if (c0 == '\\' && i + 1 < n) {
            cur.text += line[i + 1];
            i += 2;
            continue;
        }
        if (c0 == '`') {
            auto close = line.find('`', i + 1);
            if (close != std::string::npos) {
                flush();
                MdSeg s;
                s.text = line.substr(i + 1, close - i - 1);
                s.code = true;
                segs.push_back(std::move(s));
                i = close + 1;
                continue;
            }
            cur.text += '`';
            i++;
            continue;
        }
        if (line.compare(i, 2, "**") == 0) {
            auto close = line.find("**", i + 2);
            if (close != std::string::npos) {
                flush();
                MdSeg s;
                s.text = line.substr(i + 2, close - i - 2);
                s.bold = true;
                segs.push_back(std::move(s));
                i = close + 2;
                continue;
            }
            cur.text.append(line, i, 2);
            i += 2;
            continue;
        }
        if (line.compare(i, 2, "~~") == 0) {
            auto close = line.find("~~", i + 2);
            if (close != std::string::npos) {
                flush();
                MdSeg s;
                s.text = line.substr(i + 2, close - i - 2);
                s.strike = true;
                segs.push_back(std::move(s));
                i = close + 2;
                continue;
            }
            cur.text.append(line, i, 2);
            i += 2;
            continue;
        }
        if (c0 == '*' && (i + 1 >= n || line[i + 1] != '*')) {
            // 单 * 斜体；跳过指向 ** 的闭合点
            size_t close = line.find('*', i + 1);
            while (close != std::string::npos && close + 1 < n && line[close + 1] == '*')
                close = line.find('*', close + 2);
            if (close != std::string::npos) {
                flush();
                MdSeg s;
                s.text = line.substr(i + 1, close - i - 1);
                s.italic = true;
                segs.push_back(std::move(s));
                i = close + 1;
                continue;
            }
            cur.text += '*';
            i++;
            continue;
        }
        if (c0 == '[') {
            auto lp = line.find("](", i + 1);
            if (lp != std::string::npos) {
                auto rp = line.find(')', lp + 2);
                if (rp != std::string::npos) {
                    flush();
                    MdSeg a;
                    a.text = line.substr(i + 1, lp - i - 1);
                    a.bold = true;
                    a.fg = Color::Green;
                    segs.push_back(std::move(a));
                    MdSeg b;
                    b.text = line.substr(lp + 2, rp - lp - 2);
                    b.dim = true;
                    b.fg = Color::Green;
                    segs.push_back(std::move(b));
                    i = rp + 1;
                    continue;
                }
            }
            cur.text += '[';
            i++;
            continue;
        }
        int cl = md_utf8_len(c0);
        cur.text.append(line, i, (size_t)cl);
        i += (size_t)cl;
    }
    flush();
    return segs;
}

// 前缀（缩进/标记/引用条）附到分段头，dim 样式
inline void md_prefix(std::vector<MdSeg>& segs, const std::string& prefix) {
    MdSeg pre;
    pre.text = prefix;
    pre.dim = true;
    segs.insert(segs.begin(), std::move(pre));
}

// 行级解析: 标题 / 围栏 / 列表 / 引用 / 分隔线 / 段落
inline std::vector<MdBlock> md_parse(const std::string& text) {
    std::vector<MdBlock> out;
    bool in_fence = false;
    for (auto& raw : split_diff_lines(text)) {
        size_t lead = 0;
        while (lead < raw.size() && raw[lead] == ' ') lead++;
        const std::string content = raw.substr(lead);

        if (in_fence) {
            if (content.size() >= 3 &&
                (content.compare(0, 3, "```") == 0 || content.compare(0, 3, "~~~") == 0)) {
                in_fence = false;
                continue;
            }
            MdBlock b;
            b.kind = MdKind::Code;
            MdSeg s;
            s.text = content;  // 原样保留，不做行内解析
            b.segs.push_back(std::move(s));
            out.push_back(std::move(b));
            continue;
        }
        if (content.size() >= 3 &&
            (content.compare(0, 3, "```") == 0 || content.compare(0, 3, "~~~") == 0)) {
            in_fence = true;
            MdBlock b;
            b.kind = MdKind::Code;
            b.lang = md_trim(content.substr(3));
            out.push_back(std::move(b));
            continue;
        }
        if (content.empty()) {
            out.push_back(MdBlock{MdKind::Blank, {}, "", 0});
            continue;
        }
        size_t h = 0;
        while (h < content.size() && content[h] == '#') h++;
        if (h >= 1 && h <= 6 && h < content.size() && content[h] == ' ') {
            out.push_back(MdBlock{MdKind::Heading, md_inline(md_trim(content.substr(h + 1))), "", (int)h});
            continue;
        }
        if (md_is_hr(content)) {
            out.push_back(MdBlock{MdKind::HR, {}, "", 0});
            continue;
        }
        if (content[0] == '>') {
            std::string body = content.substr(1);
            if (!body.empty() && body[0] == ' ') body = body.substr(1);
            auto segs = md_inline(body);
            md_prefix(segs, "▎ ");
            out.push_back(MdBlock{MdKind::Quote, std::move(segs), "", 0});
            continue;
        }
        const char c0 = content[0];
        bool is_list = false;
        std::string marker;
        std::string body;
        if ((c0 == '-' || c0 == '*' || c0 == '+') && content.size() > 1 && content[1] == ' ') {
            is_list = true;
            marker = "• ";
            body = content.substr(2);
        } else if (c0 >= '0' && c0 <= '9') {
            size_t d = 1;
            while (d < content.size() && content[d] >= '0' && content[d] <= '9') d++;
            if (d < content.size() && content[d] == '.' && d + 1 < content.size() && content[d + 1] == ' ') {
                is_list = true;
                marker = content.substr(0, d + 1) + " ";
                body = content.substr(d + 2);
            }
        }
        if (is_list) {
            auto segs = md_inline(body);
            md_prefix(segs, std::string(std::min(lead, (size_t)8), ' ') + marker);
            out.push_back(MdBlock{MdKind::List, std::move(segs), "", 0});
            continue;
        }
        out.push_back(MdBlock{MdKind::Para, md_inline(content), "", 0});
    }
    return out;
}

// 渲染为逐行元素 + 签名（行数 = rows.size()，滚动对齐与命中测试同源）
inline std::vector<UiRow> md_rows(const std::string& src, int width) {
    std::vector<UiRow> rows;
    for (auto& b : md_parse(src)) {
        switch (b.kind) {
        case MdKind::Para:
        case MdKind::Quote:
        case MdKind::List: {
            auto r = md_styled_rows(b.segs, width);
            for (auto& u : r) rows.push_back(std::move(u));
            break;
        }
        case MdKind::Heading: {
            Color c = Color::Default;
            if (b.level <= 1) c = Color::GreenLight;
            else if (b.level == 2) c = Color::Green;
            auto r = md_styled_rows(b.segs, width);
            for (auto& u : r) {
                u.el = u.el | bold | color(c);
                rows.push_back(std::move(u));
            }
            break;
        }
        case MdKind::Code: {
            if (b.segs.empty() && b.lang.empty()) continue;  // 空围栏
            int w = std::max(1, width - 2);
            if (!b.lang.empty())
                for (auto& wl : wrap_by_width(b.lang, w))
                    rows.push_back({hbox({text("│ "), text(wl) | dim | flex}) |
                                        bgcolor(Color(Color::Palette256::Grey19)),
                                    "│ " + wl});
            for (auto& seg : b.segs)
                for (auto& wl : wrap_by_width(seg.text, w))
                    rows.push_back({hbox({text("│ "), text(wl) | color(kToolMuted) | flex}) |
                                        bgcolor(Color(Color::Palette256::Grey19)),
                                    "│ " + wl});
            break;
        }
        case MdKind::HR: {
            std::string hr;
            for (int i = 0; i < std::max(1, width); i++) hr += "\xE2\x94\x80";  // ─
            rows.push_back({text(hr) | dim, hr});
            break;
        }
        case MdKind::Blank:
            break;
        }
    }
    return rows;
}

} // namespace codis