#pragma once

// 纯逻辑工具函数：历史重放过滤、上下文压缩拼接、tool_calls 解析、token 估算。
// 全部为 inline 纯函数（无 IO/网络/全局状态），供 server.cpp 与单测共用。

#include "messages.h"
#include "tool.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace codis {
namespace context_utils {

// =============================================================================
// 历史重放过滤
// =============================================================================

// 纯空白判断（剥掉 tool_calls JSON 后模型输出可能只剩换行）
inline bool is_blank(const std::string& s) {
    for (char c : s)
        if (!std::isspace(static_cast<unsigned char>(c))) return false;
    return true;
}

// 历史重放谓词：system（压缩摘要，仅 compact 后出现）+ user + tool +
// 完整工具往返的 assistant（中转带 tool_call_id 或纯文本回复）。
inline bool is_replayable(const Message& m) {
    return m.role == "system" || m.role == "user" || m.role == "tool" ||
           (m.role == "assistant" && (m.tool_call_id || !is_blank(m.content)));
}

// 思维链挂账：reasoning 角色不入上下文（openai 兼容端点对未知角色直接 400），
// 但严格 thinking provider（如 deepseek 系）要求历史中每条 assistant 消息
// 原样回传 reasoning_content，否则 400。因此把 reasoning 内容挂到同轮（直到
// 下一条 reasoning 或新一轮 user 之前）的每条 assistant 消息上。
inline void append_with_reasoning(std::vector<Message>& out, const Message& m,
                                  std::string& pending) {
    if (m.role == "reasoning") { pending = m.content; return; }
    if (m.role == "user") pending.clear();  // 新一轮任务，上一轮思维链作废
    out.push_back(m);
    if (m.role == "assistant" && !pending.empty())
        out.back().reasoning_content = pending;
}

// 历史重放：过滤 + 思维链挂账的完整版本（task 循环与 REST /chat 共用）
inline std::vector<Message> replayable_messages(const std::vector<Message>& history) {
    std::vector<Message> out;
    std::string pending;
    for (auto& m : history)
        if (is_replayable(m) || m.role == "reasoning")
            append_with_reasoning(out, m, pending);
    return out;
}

// =============================================================================
// 上下文压缩 — 切分与拼接（LLM 摘要本身由调用方生成）
// =============================================================================

constexpr int kCompactDefaultKeep = 20;   // 尾部保留原文条数
constexpr int kCompactMinKeep = 4;        // 尾部至少保留的条数（题干 + 近期往返）
constexpr int kCompactMaxKeep = 100;

inline int clamp_keep(int keep) {
    return std::clamp(keep, kCompactMinKeep, kCompactMaxKeep);
}

struct CompactSplit {
    std::vector<Message> prefix;  // 头部（将被摘要）
    std::vector<Message> tail;    // 尾部原文窗口
};

// 按 keep 切分历史；历史过短（<= keep+1 条）返回 nullopt（无需压缩）。
inline std::optional<CompactSplit> split_history(const std::vector<Message>& history, int keep) {
    keep = clamp_keep(keep);
    if (history.size() <= static_cast<size_t>(keep) + 1) return std::nullopt;
    const size_t split = history.size() - static_cast<size_t>(keep);
    return CompactSplit{
        {history.begin(), history.begin() + static_cast<long>(split)},
        {history.begin() + static_cast<long>(split), history.end()}};
}

// 新历史 = 摘要(system) + 题干(prefix 首条 user) + 尾部原文
inline std::vector<Message> build_compacted_history(const CompactSplit& split,
                                                    const std::string& summary) {
    std::vector<Message> compacted;
    compacted.push_back({"system", "上下文摘要（历史压缩）:\n" + summary});
    for (auto& m : split.prefix)
        if (m.role == "user") { compacted.push_back(m); break; }
    for (auto& m : split.tail) compacted.push_back(m);
    return compacted;
}

// =============================================================================
// token 估算 — len/4 近似（ASCII 为主 + 中文按 ~1 token/字裕量）
// =============================================================================

inline int64_t est_tokens(const std::vector<Message>& msgs) {
    int64_t n = 0;
    for (auto& m : msgs) n += static_cast<int64_t>(m.content.size()) / 4;
    return n;
}

// =============================================================================
// Tool call 提取 — 从 token 流中解析
// =============================================================================

// 定位 content 中 tool_calls 最外层 JSON 的字节区间 [begin, end)（含 ```json 围栏）。
// 找不到返回 {npos, npos}；JSON 被截断时剥到末尾（保证文本部分可安全取出）。
inline std::pair<size_t, size_t> tool_calls_json_span(const std::string& content) {
    auto pos = content.find("\"tool_calls\"");
    if (pos == std::string::npos) return {std::string::npos, std::string::npos};

    // 优先从 markdown 代码块中提取
    auto md_start = content.find("```json");
    if (md_start != std::string::npos && md_start < pos) {
        auto md_end = content.find("```", md_start + 7);
        if (md_end == std::string::npos) return {md_start, content.size()};
        return {md_start, md_end + 3};
    }

    // LLM 可能在 tool_calls JSON 前输出文本，找到包含 "tool_calls" 的最外层 JSON 对象
    // 用括号栈定位闭合位置（截断/漏写右括号时剥到末尾）
    auto brace = content.rfind('{', pos);
    if (brace == std::string::npos) return {std::string::npos, std::string::npos};

    std::string stack;
    bool in_string = false;
    bool escaped = false;
    for (auto i = brace; i < content.size(); i++) {
        char c = content[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\') { escaped = true; continue; }
        if (c == '"') { in_string = !in_string; continue; }
        if (in_string) continue;
        if (c == '{') { stack += '{'; }
        else if (c == '[') { stack += '['; }
        else if (c == '}') {
            if (!stack.empty() && stack.back() == '{') stack.pop_back();
            if (stack.empty()) return {brace, i + 1};
        } else if (c == ']') {
            // 数组内还有未闭合对象：先补 } 再闭合 ]
            while (!stack.empty() && stack.back() == '{') stack.pop_back();
            if (!stack.empty() && stack.back() == '[') stack.pop_back();
            if (stack.empty()) return {brace, i + 1};
        }
    }
    return {brace, content.size()};
}

// 从 LLM 输出中解析 tool_calls。优先取 markdown ```json 块；否则用括号栈
// 自动补齐模型偶尔截断/漏写的右括号（如缺 } 或提前 ]）。
inline std::vector<ToolCall> extract_tool_calls(const std::string& content) {
    std::vector<ToolCall> calls;
    auto pos = content.find("\"tool_calls\"");
    if (pos == std::string::npos) return calls;

    std::string json_str;

    // 优先从 markdown 代码块中提取
    auto md_start = content.find("```json");
    if (md_start != std::string::npos) {
        md_start += 7;
        auto md_end = content.find("```", md_start);
        if (md_end != std::string::npos)
            json_str = content.substr(md_start, md_end - md_start);
    } else {
        // LLM 可能在 tool_calls JSON 前输出文本，找到包含 "tool_calls" 的最外层 JSON 对象
        auto brace = content.rfind('{', pos);
        if (brace == std::string::npos) return calls;
        std::string stack;
        std::string fixed;
        bool in_string = false;
        bool escaped = false;
        auto close_outer = [&] {
            if (stack.empty()) return;
            while (!fixed.empty() && fixed.back() == ',') fixed.pop_back();
            while (!stack.empty()) {
                fixed += stack.back() == '{' ? '}' : ']';
                stack.pop_back();
            }
        };
        for (auto i = brace; i < content.size(); i++) {
            char c = content[i];
            if (escaped) { escaped = false; fixed += c; continue; }
            if (c == '\\') { escaped = true; fixed += c; continue; }
            if (c == '"') { in_string = !in_string; fixed += c; continue; }
            if (in_string) { fixed += c; continue; }
            if (c == '{') { stack += '{'; fixed += c; }
            else if (c == '[') { stack += '['; fixed += c; }
            else if (c == '}') {
                while (!fixed.empty() && fixed.back() == ',') fixed.pop_back();
                if (!stack.empty() && stack.back() == '{') { stack.pop_back(); fixed += c; }
                if (stack.empty()) break;
            } else if (c == ']') {
                // 数组内还有未闭合对象：先补 } 再闭合 ]
                while (!fixed.empty() && fixed.back() == ',') fixed.pop_back();
                while (!stack.empty() && stack.back() == '{') { fixed += '}'; stack.pop_back(); }
                if (!stack.empty() && stack.back() == '[') { stack.pop_back(); fixed += c; }
                if (stack.empty()) break;
            } else if (c != ',') {
                fixed += c;
            } else if (!fixed.empty() && fixed.back() != ',') {
                fixed += c;
            }
        }
        close_outer();
        json_str = fixed;
    }

    try {
        auto j = json::parse(json_str);
        if (j.contains("tool_calls")) {
            for (auto& tc : j["tool_calls"]) {
                ToolCall call;
                call.id = tc.value("id", "");
                auto& func = tc["function"];
                call.name = func.value("name", "");
                call.arguments = func.value("arguments", json::object());
                calls.push_back(call);
            }
        }
    } catch (...) {}
    return calls;
}

} // namespace context_utils
} // namespace codis