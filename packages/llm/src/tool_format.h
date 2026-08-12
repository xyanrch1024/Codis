#pragma once

#include "types.h"

#include <string>
#include <vector>

namespace opencode {

// 将工具调用渲染为可读文本，如:
//   bash   → "bash python3 calculator.py"
//   read   → "read /path/to/file.cpp"
//   write  → "write /path/to/file.cpp"
//   edit   → "edit /path/to/file.cpp"
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
        if (auto p = str_arg("filePath")) return "write " + *p;
    } else if (name == "edit") {
        if (auto p = str_arg("filePath")) return "edit " + *p;
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

// 截断 tool 执行输出用于 UI 展示:
//   - 最多保留 max_lines 行，超出追加 "... (N more lines)"
//   - 单行超过 max_line_len 字符时截断
//   空内容返回空串
inline std::string truncate_tool_output(const std::string& content,
                                        size_t max_lines = 20,
                                        size_t max_line_len = 500) {
    std::vector<std::string> kept;
    size_t total = 0;
    size_t pos = 0;
    while (pos < content.size()) {
        auto nl = content.find('\n', pos);
        size_t end = (nl == std::string::npos) ? content.size() : nl;
        auto line = content.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        total++;
        if (kept.size() < max_lines) {
            if (line.size() > max_line_len)
                line = line.substr(0, max_line_len) + "...";
            kept.push_back(std::move(line));
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    if (total > kept.size())
        kept.push_back("... (" + std::to_string(total - kept.size()) + " more lines)");

    std::string out;
    for (size_t i = 0; i < kept.size(); ++i) {
        if (i) out += '\n';
        out += kept[i];
    }
    return out;
}

} // namespace opencode
