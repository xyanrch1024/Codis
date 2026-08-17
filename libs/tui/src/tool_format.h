#pragma once

#include "messages.h"

#include <string>
#include <string_view>
#include <vector>
#include <algorithm>

namespace codis {

// =============================================================================
// edit 工具的统一 diff 渲染（LCS 对齐，输出类 git diff）
// =============================================================================

inline std::vector<std::string> split_diff_lines(const std::string& s) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < s.size()) {
        auto nl = s.find('\n', pos);
        size_t end = (nl == std::string::npos) ? s.size() : nl;
        lines.push_back(s.substr(pos, end - pos));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

// unified diff 每行的展示类别（TUI 逐行着色用）
enum class DiffLineKind { Head, Hunk, Added, Removed, Context, Plain };

inline DiffLineKind classify_diff_line(std::string_view line) {
    if (line.empty()) return DiffLineKind::Plain;
    if (line.substr(0, 2) == "@@") return DiffLineKind::Hunk;
    if (line.substr(0, 3) == "---" || line.substr(0, 3) == "+++") return DiffLineKind::Head;
    switch (line.front()) {
        case '+': return DiffLineKind::Added;
        case '-': return DiffLineKind::Removed;
        case ' ': return DiffLineKind::Context;
    }
    return DiffLineKind::Plain;
}

// 将 oldString/newString 渲染为 unified diff:
//   edit <path>
//   --- a/<path>
//   +++ b/<path>
//   @@ -1,3 +1,4 @@
//    int lives = 3;
//   -while (alive) {
//   +while (alive && lives > 0) {
//       move();
//   +    lives--;
//    }
inline std::string format_edit_diff(const std::string& path,
                                    const std::string& old_str,
                                    const std::string& new_str) {
    auto o = split_diff_lines(old_str);
    auto n = split_diff_lines(new_str);
    int no = (int)o.size(), nn = (int)n.size();

    // git diff 风格的 a/b 头使用相对路径（去掉开头的 /）
    std::string hdr = path;
    if (hdr.size() > 1 && hdr.front() == '/') hdr = hdr.substr(1);

    std::vector<std::string> out = {"edit " + path,
                                    "--- a/" + hdr,
                                    "+++ b/" + hdr};

    // LCS 填表（从后往前）
    std::vector<std::vector<int>> dp(no + 1, std::vector<int>(nn + 1, 0));
    for (int i = no - 1; i >= 0; i--)
        for (int j = nn - 1; j >= 0; j--)
            dp[i][j] = o[i] == n[j] ? dp[i + 1][j + 1] + 1 : std::max(dp[i + 1][j], dp[i][j + 1]);

    // 回溯生成操作序列
    struct Op { char kind; int old_no; int new_no; };
    std::vector<Op> ops;
    int i = 0, j = 0;
    while (i < no && j < nn) {
        if (o[i] == n[j]) { ops.push_back({' ', i, j}); i++; j++; }
        else if (dp[i + 1][j] >= dp[i][j + 1]) { ops.push_back({'-', i, -1}); i++; }
        else { ops.push_back({'+', -1, j}); j++; }
    }
    while (i < no) { ops.push_back({'-', i, -1}); i++; }
    while (j < nn) { ops.push_back({'+', -1, j}); j++; }

    // 带上下文的分块：连续改动间隔 <= 2*ctx 行时合并为一个 hunk
    const int ctx = 3;
    std::vector<std::pair<int, int>> hunks;
    int k = 0;
    while (k < (int)ops.size()) {
        if (ops[k].kind == ' ') { k++; continue; }
        int start = (k >= ctx) ? k - ctx : 0;
        int end = k;
        for (;;) {
            while (end < (int)ops.size() && ops[end].kind != ' ') end++;
            int eq_start = end;
            while (end < (int)ops.size() && ops[end].kind == ' ') end++;
            int eq = end - eq_start;
            if (eq > ctx || end == (int)ops.size()) {
                end = eq_start + std::min(eq, ctx);
                break;
            }
        }
        hunks.push_back({start, end});
        k = end;
    }

    if (hunks.empty()) {
        out.push_back("(内容无变化)");
    } else {
        for (auto& [hs, he] : hunks) {
            int old_start = 1, new_start = 1;
            for (int t = 0; t < hs; t++) {
                if (ops[t].kind != '+') old_start++;
                if (ops[t].kind != '-') new_start++;
            }
            int old_count = 0, new_count = 0;
            for (int t = hs; t < he; t++) {
                if (ops[t].kind != '+') old_count++;
                if (ops[t].kind != '-') new_count++;
            }
            out.push_back("@@ -" + std::to_string(old_start) + "," + std::to_string(old_count) +
                          " +" + std::to_string(new_start) + "," + std::to_string(new_count) + " @@");
            for (int t = hs; t < he; t++) {
                char c = ops[t].kind;
                const std::string& ln = (c == '+') ? n[ops[t].new_no] : o[ops[t].old_no];
                out.push_back(std::string(1, c) + ln);
            }
        }
    }

    std::string result;
    for (auto& line : out) result += line + "\n";
    if (!result.empty() && result.back() == '\n') result.pop_back();
    return result;
}

// 将 write 的 content 渲染为新建文件的 unified diff:
//   write /tmp/new.cpp
//   --- /dev/null
//   +++ b/tmp/new.cpp
//   @@ -0,0 +1,2 @@
//   +#include <iostream>
//   +int main() {}
inline std::string format_write(const std::string& path, const std::string& content) {
    auto lines = split_diff_lines(content);
    int count = (int)lines.size();

    std::string hdr = path;
    if (hdr.size() > 1 && hdr.front() == '/') hdr = hdr.substr(1);

    std::string out = "write " + path + "\n--- /dev/null\n+++ b/" + hdr;
    if (count == 0) {
        out += "\n(空文件)";
        return out;
    }
    out += "\n@@ -0,0 +1," + std::to_string(count) + " @@";
    for (auto& ln : lines) out += "\n+" + ln;
    return out;
}

// 将工具调用渲染为可读文本，如:
//   bash   → "bash python3 calculator.py"
//   read   → "read /path/to/file.cpp"
//   write  → "write /path/to/file.cpp" + 内容（新建文件 diff）
//   edit   → "edit /path/to/file.cpp" 的 unified diff
//   glob   → "glob **/*.cpp"
//   grep   → "grep main"
//   plugin → "my_tool <arg>"
inline std::string format_tool_call(const std::string& name, const json& args) {
    auto str_arg = [&](const char* key) -> std::optional<std::string> {
        if (args.contains(key) && args[key].is_string())
            return args[key].get<std::string>();
        return std::nullopt;
    };

    if (name == "bash") {
        if (auto cmd = str_arg("command")) return "bash " + *cmd;
    } else if (name == "read") {
        if (auto p = str_arg("filePath")) return "read " + *p;
    } else if (name == "write") {
        if (auto p = str_arg("filePath")) {
            auto content = str_arg("content");
            if (content) return format_write(*p, *content);
            return "write " + *p;
        }
    } else if (name == "edit") {
        if (auto p = str_arg("filePath")) {
            auto old_str = str_arg("oldString");
            auto new_str = str_arg("newString");
            if (old_str || new_str)
                return format_edit_diff(*p, old_str.value_or(""), new_str.value_or(""));
            return "edit " + *p;
        }
    } else if (name == "glob") {
        if (auto pat = str_arg("pattern")) return "glob " + *pat;
    } else if (name == "grep") {
        if (auto pat = str_arg("pattern")) return "grep " + *pat;
    }

    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.value().is_string())
            return name + " " + it.value().get<std::string>();
    }

    return name;
}

// =============================================================================
// openCode 风格的每工具展示元数据（TUI 渲染用）
// =============================================================================

struct ToolDisplay {
    std::string icon;         // 图标（"$", "→", "←", "✱", "⚙"）
    std::string label;        // 完成后内联文案（不含 icon），如 "Read /a.cpp"
    std::string pending;      // pending 文案，如 "Reading file..."
    std::string block_title;  // 块布局标题（可为空），如 "# Wrote /a.cpp"
    bool block = false;       // 结果到达后是否用块布局（write/edit 恒真）
};

inline ToolDisplay tool_display(const std::string& name, const json& args) {
    auto str_arg = [&](const char* key) -> std::optional<std::string> {
        if (args.contains(key) && args[key].is_string())
            return args[key].get<std::string>();
        return std::nullopt;
    };

    ToolDisplay d;
    if (name == "bash") {
        d.icon = "$";
        d.label = str_arg("command").value_or("");
        d.pending = "Writing command...";
        d.block = true;
    } else if (name == "read") {
        d.icon = "→";
        d.label = "Read " + str_arg("filePath").value_or("?");
        d.pending = "Reading file...";
    } else if (name == "write") {
        d.icon = "←";
        auto p = str_arg("filePath").value_or("");
        d.label = "Write " + p;
        d.pending = "Preparing write...";
        d.block = true;
        d.block_title = "# Wrote " + p;
    } else if (name == "edit") {
        d.icon = "←";
        auto p = str_arg("filePath").value_or("");
        d.label = "Edit " + p;
        d.pending = "Preparing edit...";
        d.block = true;
        d.block_title = "← Edit " + p;
    } else if (name == "glob") {
        d.icon = "✱";
        std::string t = "Glob \"" + str_arg("pattern").value_or("?") + "\"";
        if (auto path = str_arg("path")) t += " in " + *path;
        d.label = std::move(t);
        d.pending = "Finding files...";
    } else if (name == "grep") {
        d.icon = "✱";
        std::string t = "Grep \"" + str_arg("pattern").value_or("?") + "\"";
        if (auto path = str_arg("path")) t += " in " + *path;
        d.label = std::move(t);
        d.pending = "Searching content...";
    } else {
        d.icon = "⚙";
        for (auto it = args.begin(); it != args.end(); ++it) {
            if (it.value().is_string()) { d.label = name + " " + it.value().get<std::string>(); break; }
        }
        if (d.label.empty()) d.label = name;
        d.pending = "Running...";
    }
    return d;
}

} // namespace codis
