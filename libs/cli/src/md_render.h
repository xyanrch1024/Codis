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
// 行数估算与渲染同源（同一 parse + wrap），保证滚动对齐不漂移。
// =============================================================================

struct MdSeg {
    std::string text;
    Color fg = Color::Default;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool code = false;   // 行内代码: 纸片底色（与代码块同色系）
    bool strike = false;
};

enum class MdKind { Para, Heading, Code, Quote, List, HR, Blank };

struct MdBlock {
    MdKind kind;
    std::vector<MdSeg> segs;   // 一段逻辑行（无 \n）
    std::string lang;          // Code: 围栏语言
    int level = 0;             // Heading 级别
};

inline int md_utf8_len(char c) {
    unsigned char u = (unsigned char)c;
    if (u >= 0xF0) return 4;
    if (u >= 0xE0) return 3;
    if (u >= 0xC0) return 2;
    return 1;
}

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

// 样式分段 → 拼接文本（可选输出每段字节边界）
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

// 样式分段 → 按宽度换行成多行（每行一条 hbox<分段>，单行元素）
// 与 md_row_count 同源：同一拼接文本 + wrap_by_width，行数必然一致。
inline Elements md_styled_rows(const std::vector<MdSeg>& segs, int width) {
    std::vector<size_t> bounds;
    std::string text = md_text_of(segs, &bounds);
    Elements rows;
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
        rows.push_back(hbox(std::move(parts)));
    }
    return rows;
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

// 渲染为 Element（上游卡片已提供底色；代码块复用 tool_block 视觉）
inline Element md_render(const std::string& src, int width) {
    Elements els;
    for (auto& b : md_parse(src)) {
        switch (b.kind) {
        case MdKind::Para:
        case MdKind::Quote:
        case MdKind::List:
            els.push_back(vbox(md_styled_rows(b.segs, width)));
            break;
        case MdKind::Heading: {
            Color c = Color::Default;
            if (b.level <= 1) c = Color::GreenLight;
            else if (b.level == 2) c = Color::Green;
            els.push_back(vbox(md_styled_rows(b.segs, width)) | bold | color(c));
            break;
        }
        case MdKind::Code: {
            if (b.segs.empty() && b.lang.empty()) continue;  // 空围栏
            Elements lines;
            if (!b.lang.empty())
                for (auto& wl : wrap_by_width(b.lang, std::max(1, width - 2)))
                    lines.push_back(text(wl) | dim);
            for (auto& seg : b.segs)
                for (auto& wl : wrap_by_width(seg.text, std::max(1, width - 2)))
                    lines.push_back(text(wl) | color(kToolMuted));
            els.push_back(tool_block(std::move(lines)));
            break;
        }
        case MdKind::HR: {
            std::string hr;
            for (int i = 0; i < std::max(1, width); i++) hr += "\xE2\x94\x80";  // ─
            els.push_back(text(hr) | dim);
            break;
        }
        case MdKind::Blank:
            break;
        }
    }
    return vbox(std::move(els));
}

// 行数估算（与 md_render 同源：同一 parse + 同一 wrap 宽度规则）
inline int md_row_count(const std::string& text, int width) {
    int n = 0;
    for (auto& b : md_parse(text)) {
        switch (b.kind) {
        case MdKind::Para:
        case MdKind::Heading:
        case MdKind::Quote:
        case MdKind::List:
            n += (int)wrap_by_width(md_text_of(b.segs, nullptr), width).size();
            break;
        case MdKind::Code: {
            int w = std::max(1, width - 2);
            if (!b.lang.empty()) n += (int)wrap_by_width(b.lang, w).size();
            for (auto& seg : b.segs) n += (int)wrap_by_width(seg.text, w).size();
            break;
        }
        case MdKind::HR:
            n += 1;
            break;
        case MdKind::Blank:
            break;
        }
    }
    return n;
}

} // namespace codis