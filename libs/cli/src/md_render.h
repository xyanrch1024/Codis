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

enum class MdKind { Para, Heading, Code, Quote, List, HR, Blank, Table };

// GFM 表格（跨行块）：cells 行优先 r*cols+c，align 每列 0=左 1=中 2=右
struct MdTable {
    std::vector<std::vector<MdSeg>> cells;
    std::vector<int> align;
    int cols = 0;
    int rows = 0;
};

struct MdBlock {
    MdKind kind;
    std::vector<MdSeg> segs;   // 一段逻辑行（无 \n）
    std::string lang;          // Code: 围栏语言
    int level = 0;             // Heading 级别
    MdTable table;             // Table: 表格数据
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

// 表格行 → 单元格（去首尾管道、按 | 分割、忽略 \| 转义、各单元 trim）
inline std::vector<std::string> md_split_cells(const std::string& line) {
    std::string s = md_trim(line);
    if (!s.empty() && s.front() == '|') s = s.substr(1);
    if (!s.empty() && s.back() == '|') s.pop_back();
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find('|', start);
        while (p != std::string::npos && p > start && s[p - 1] == '\\')
            p = s.find('|', p + 1);
        out.push_back(md_trim(s.substr(start, p == std::string::npos
                                           ? std::string::npos : p - start)));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

// 分隔行判定：| --- | :--: | ...；成功时填充 align[cell]（0/1/2）
inline bool md_sep_align(const std::string& line, std::vector<int>* align) {
    auto cells = md_split_cells(line);
    if (cells.empty()) return false;
    align->clear();
    for (auto& c : cells) {
        if (c.empty()) return false;
        int dashes = 0;
        for (char ch : c) {
            if (ch == '-') { dashes++; continue; }
            if (ch != ':' && ch != ' ') return false;
        }
        if (dashes < 1) return false;
        bool l = c.front() == ':';
        bool r = c.back() == ':';
        align->push_back((l && r) ? 1 : (r ? 2 : 0));
    }
    return true;
}

// 按显示宽度截断（不切 UTF-8 字符），尾部加 …
inline std::string md_trunc_w(const std::string& s, int w) {
    std::string out;
    int ow = 0;
    size_t i = 0;
    while (i < s.size()) {
        int cl = md_utf8_len(s[i]);
        if (i + (size_t)cl > s.size()) cl = 1;
        int cw = string_width(std::string_view(s.data() + i, (size_t)cl));
        if (ow + cw > w) break;
        out.append(s, i, (size_t)cl);
        ow += cw;
        i += (size_t)cl;
    }
    if (i < s.size()) out += "\xE2\x80\xA6";  // …
    return out;
}

// 单元格按宽度截断，保留样式分段（截断字节落在某段中间时裁掉其后段）
inline std::vector<MdSeg> md_fit_cell(const std::vector<MdSeg>& cell, int w) {
    std::string full = md_text_of(cell, nullptr);
    if (string_width(full) <= w) return cell;
    std::string cut = md_trunc_w(full, w);
    std::vector<MdSeg> out;
    size_t used = 0;
    for (auto& s : cell) {
        if (used >= cut.size()) break;
        MdSeg ns = s;
        size_t rem = cut.size() - used;
        if (ns.text.size() > rem) ns.text = cut.substr(used, rem);
        used += ns.text.size();
        out.push_back(std::move(ns));
    }
    return out;
}

// 表格 → 边框行 + 数据行（表头加粗，列宽自适应 + 逐列上限，超宽截断）
inline std::vector<UiRow> md_table_rows(const MdTable& t, int width) {
    std::vector<UiRow> rows;
    if (t.rows <= 0 || t.cols <= 0) return rows;
    auto cell_text = [&](int r, int c) {
        return md_text_of(t.cells[r * t.cols + c], nullptr);
    };
    std::vector<int> cw(t.cols, 0);
    for (int r = 0; r < t.rows; r++)
        for (int c = 0; c < t.cols; c++)
            cw[c] = std::max(cw[c], string_width(cell_text(r, c)));
    const int kMaxCol = 24;
    for (auto& w : cw) w = std::min(w, kMaxCol);
    int total = 3 * t.cols + 2;  // 列宽和 + 边框/分隔符（与下方行构造一致）
    for (int c = 0; c < t.cols; c++) total += cw[c];
    int budget = std::max(12, width - 4);
    int guard = 0;
    while (total > budget && guard++ < t.cols * 10) {
        int bi = 0;
        for (int c = 1; c < t.cols; c++) if (cw[c] > cw[bi]) bi = c;
        if (cw[bi] <= 2) break;
        int cut = std::min(4, cw[bi] - 2);
        cw[bi] -= cut;
        total -= cut;
    }
    auto border = [&](const char* l, const char* m, const char* r) {
        std::string s = l;
        for (int c = 0; c < t.cols; c++) {
            for (int i = 0; i < cw[c] + 2; i++) s += "\xE2\x94\x80";  // ─
            s += (c + 1 < t.cols) ? m : r;
        }
        return s;
    };
    std::string hr_top = border("\xE2\x94\x8C", "\xE2\x94\xAC", "\xE2\x94\x90");       // ┌┬┐
    std::string hr_mid = border("\xE2\x94\x9C", "\xE2\x94\xBC", "\xE2\x94\xA4");       // ├┼┤
    std::string hr_bot = border("\xE2\x94\x94", "\xE2\x94\xB4", "\xE2\x94\x98");       // └┴┘
    rows.push_back({text(hr_top) | dim, hr_top});
    for (int r = 0; r < t.rows; r++) {
        Elements parts;
        std::string sig = "\xE2\x94\x82 ";  // │
        parts.push_back(text(sig) | dim);
        for (int c = 0; c < t.cols; c++) {
            auto segs = md_fit_cell(t.cells[r * t.cols + c], cw[c]);
            if (r == 0) for (auto& s : segs) s.bold = true;
            std::string txt = md_text_of(segs, nullptr);
            int pad_rem = cw[c] - string_width(txt);
            int padl = (t.align[c] == 1) ? pad_rem / 2 : ((t.align[c] == 2) ? pad_rem : 0);
            Elements cell_els;
            if (padl > 0) cell_els.push_back(text(std::string(padl, ' ')));
            for (auto& s : segs) cell_els.push_back(md_seg_el(s));
            if (pad_rem - padl > 0) cell_els.push_back(text(std::string(pad_rem - padl, ' ')));
            parts.push_back(hbox(std::move(cell_els)));
            std::string sep = (c + 1 < t.cols) ? " │ " : " │";
            parts.push_back(text(sep) | dim);
            sig += txt + sep;
        }
        rows.push_back({hbox(std::move(parts)), sig});
        if (r == 0) rows.push_back({text(hr_mid) | dim, hr_mid});
    }
    rows.push_back({text(hr_bot) | dim, hr_bot});
    return rows;
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

// 行级解析: 标题 / 围栏 / 表格 / 列表 / 引用 / 分隔线 / 段落
inline std::vector<MdBlock> md_parse(const std::string& text) {
    std::vector<MdBlock> out;
    bool in_fence = false;
    auto lines = split_diff_lines(text);
    size_t idx = 0;
    while (idx < lines.size()) {
        auto& raw = lines[idx];
        size_t lead = 0;
        while (lead < raw.size() && raw[lead] == ' ') lead++;
        const std::string content = raw.substr(lead);

        if (in_fence) {
            if (content.size() >= 3 &&
                (content.compare(0, 3, "```") == 0 || content.compare(0, 3, "~~~") == 0)) {
                in_fence = false;
            } else {
                MdBlock b;
                b.kind = MdKind::Code;
                MdSeg s;
                s.text = content;  // 原样保留，不做行内解析
                b.segs.push_back(std::move(s));
                out.push_back(std::move(b));
            }
            idx++;
            continue;
        }
        if (content.size() >= 3 &&
            (content.compare(0, 3, "```") == 0 || content.compare(0, 3, "~~~") == 0)) {
            in_fence = true;
            MdBlock b;
            b.kind = MdKind::Code;
            b.lang = md_trim(content.substr(3));
            out.push_back(std::move(b));
            idx++;
            continue;
        }
        if (content.empty()) {
            out.push_back(MdBlock{MdKind::Blank, {}, "", 0});
            idx++;
            continue;
        }
        size_t h = 0;
        while (h < content.size() && content[h] == '#') h++;
        if (h >= 1 && h <= 6 && h < content.size() && content[h] == ' ') {
            out.push_back(MdBlock{MdKind::Heading, md_inline(md_trim(content.substr(h + 1))), "", (int)h});
            idx++;
            continue;
        }
        if (md_is_hr(content)) {
            out.push_back(MdBlock{MdKind::HR, {}, "", 0});
            idx++;
            continue;
        }
        if (content[0] == '>') {
            std::string body = content.substr(1);
            if (!body.empty() && body[0] == ' ') body = body.substr(1);
            auto segs = md_inline(body);
            md_prefix(segs, "▎ ");
            out.push_back(MdBlock{MdKind::Quote, std::move(segs), "", 0});
            idx++;
            continue;
        }

        // 表格：本行含 | 且下一行是分隔行 → 收集连续表行
        if (content.find('|') != std::string::npos && idx + 1 < lines.size()) {
            std::vector<int> align;
            std::string next = lines[idx + 1];
            size_t nl = 0;
            while (nl < next.size() && next[nl] == ' ') nl++;
            if (md_sep_align(next.substr(nl), &align)) {
                // 收集阶段按行存储（行内可并列扩展），结束时统一平铺
                std::vector<std::vector<std::vector<MdSeg>>> rows;
                int cols = 0;
                auto header = md_split_cells(content);
                cols = std::max(cols, (int)header.size());
                rows.emplace_back();
                for (auto& c : header) rows.back().push_back(md_inline(c));
                idx += 2;  // 越过表头 + 分隔行
                while (idx < lines.size()) {
                    auto& rl = lines[idx];
                    size_t ls = 0;
                    while (ls < rl.size() && rl[ls] == ' ') ls++;
                    std::string body = rl.substr(ls);
                    if (body.empty()) break;
                    if (body.size() >= 3 &&
                        (body.compare(0, 3, "```") == 0 || body.compare(0, 3, "~~~") == 0))
                        break;  // 围栏切换，留待外层处理
                    if (body.find('|') == std::string::npos) break;
                    auto cells = md_split_cells(body);
                    cols = std::max(cols, (int)cells.size());
                    rows.emplace_back();
                    for (auto& c : cells) rows.back().push_back(md_inline(c));
                    idx++;
                }
                MdTable t;
                t.cols = cols;
                t.rows = (int)rows.size();
                for (auto& row : rows) {
                    for (int c = 0; c < cols; c++)
                        t.cells.push_back(c < (int)row.size() ? row[c] : std::vector<MdSeg>{});
                }
                t.align = std::move(align);
                while ((int)t.align.size() < t.cols) t.align.push_back(0);
                out.push_back(MdBlock{MdKind::Table, {}, "", 0, std::move(t)});
                continue;
            }
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
            idx++;
            continue;
        }
        out.push_back(MdBlock{MdKind::Para, md_inline(content), "", 0});
        idx++;
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
        case MdKind::Table: {
            auto r = md_table_rows(b.table, width);
            for (auto& u : r) rows.push_back(std::move(u));
            break;
        }
        case MdKind::Blank:
            break;
        }
    }
    return rows;
}

} // namespace codis