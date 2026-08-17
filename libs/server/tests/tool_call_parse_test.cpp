// tool_calls 解析单测：markdown 块、文本前缀、截断 JSON 自动补括号、span 定位。

#include "context_utils.h"
#include "test_util.h"

#include <string>
#include <vector>

using namespace codis;
using namespace codis::context_utils;

namespace {

std::string join_names(const std::vector<ToolCall>& calls) {
    std::string s;
    for (auto& c : calls) {
        if (!s.empty()) s += ",";
        s += c.name + ":" + c.id;
    }
    return s;
}

} // namespace

void run_tool_call_parse_tests() {
    // 无 tool_calls → 空
    CHECK(extract_tool_calls("").empty());
    CHECK(extract_tool_calls("just text").empty());
    CHECK(extract_tool_calls("{}").empty());

    // markdown ```json 块，单个调用
    {
        auto calls = extract_tool_calls(
            "```json\n{\"tool_calls\": [{\"id\":\"call_1\",\"type\":\"function\","
            "\"function\":{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}}]}\n```");
        CHECK(calls.size() == 1);
        CHECK(calls[0].id == "call_1");
        CHECK(calls[0].name == "bash");
        CHECK(calls[0].arguments["command"] == "ls");
    }

    // markdown 块，多个调用
    {
        auto calls = extract_tool_calls(
            "{\"tool_calls\": ["
            "{\"id\":\"a\",\"function\":{\"name\":\"read\",\"arguments\":{\"path\":\"x\"}}},"
            "{\"id\":\"b\",\"function\":{\"name\":\"grep\",\"arguments\":{}}}]}");
        CHECK(calls.size() == 2);
        CHECK(join_names(calls) == "read:a,grep:b");
        CHECK(calls[1].arguments.empty());  // 缺 arguments → 空对象
    }

    // markdown 块前后带文本
    {
        auto calls = extract_tool_calls(
            "让我先看一下：\n```json\n{\"tool_calls\": [{\"id\":\"c1\","
            "\"function\":{\"name\":\"glob\",\"arguments\":{\"pattern\":\"**/*.cpp\"}}}]}\n```\n好的。");
        CHECK(calls.size() == 1);
        CHECK(calls[0].name == "glob");
    }

    // 无围栏：JSON 前有文本，括号栈定位外层对象
    {
        auto calls = extract_tool_calls(
            "text before {\"tool_calls\": [{\"id\":\"x\","
            "\"function\":{\"name\":\"edit\",\"arguments\":{\"oldString\":\"a\",\"newString\":\"b\"}}}]}");
        CHECK(calls.size() == 1);
        CHECK(calls[0].name == "edit");
    }

    // 截断 JSON：漏写右括号（模型常见行为）→ 自动补齐
    {
        auto calls = extract_tool_calls(
            "{\"tool_calls\": [{\"id\":\"t1\","
            "\"function\":{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}}]");
        CHECK(calls.size() == 1);
        CHECK(calls[0].name == "bash");
    }

    // 截断 JSON：数组提前闭合缺对象
    {
        auto calls = extract_tool_calls(
            "{\"tool_calls\": [{\"id\":\"t2\","
            "\"function\":{\"name\":\"write\",\"arguments\":{\"path\":\"f\"}}}");
        CHECK(calls.size() == 1);
        CHECK(calls[0].name == "write");
    }

    // 字符串中途截断（如 "command":"g 后直接断流）：括号栈只能补括号、
    // 补不了未闭合字符串 → 解析失败返回空（原实现行为，服务端走 malformed 重试路径）
    {
        auto calls = extract_tool_calls(
            "{\"tool_calls\": [{\"id\":\"t3\","
            "\"function\":{\"name\":\"bash\",\"arguments\":{\"command\":\"g");
        CHECK(calls.empty());
    }

    // 非法 JSON（解析失败）→ 空，不抛异常
    CHECK(extract_tool_calls("{\"tool_calls\": [oops").empty());

    // ---- tool_calls_json_span ----
    {
        auto [b, e] = tool_calls_json_span("no calls here");
        CHECK(b == std::string::npos && e == std::string::npos);
    }
    {
        std::string s = "pre\n```json\n{\"tool_calls\": []}\n```\npost";
        auto [b, e] = tool_calls_json_span(s);
        CHECK(b != std::string::npos && e != std::string::npos);
        CHECK(s.substr(b, e - b).find("tool_calls") != std::string::npos);
        CHECK(s.substr(b, e - b).rfind("```") != std::string::npos);
    }
    {
        std::string s = "text {\"tool_calls\": [{\"id\":\"1\",\"function\":{\"name\":\"n\",\"arguments\":{}}}]} tail";
        auto [b, e] = tool_calls_json_span(s);
        CHECK(b != std::string::npos && e != std::string::npos);
        CHECK(s.substr(b, e - b).find("tool_calls") != std::string::npos);
        CHECK(s.substr(0, b).find("text") != std::string::npos);   // 文本前缀保留
        CHECK(s.substr(e).find("tail") != std::string::npos);      // 尾部文本保留
    }
    // 截断时剥到末尾
    {
        std::string s = "{\"tool_calls\": [{\"id\":\"1\",\"function\":{\"name\":\"n\"";
        auto [b, e] = tool_calls_json_span(s);
        CHECK(b != std::string::npos);
        CHECK(e == s.size());
    }
    // 未闭合 markdown 围栏 → 剥到末尾
    {
        std::string s = "```json\n{\"tool_calls\": []";
        auto [b, e] = tool_calls_json_span(s);
        CHECK(b != std::string::npos);
        CHECK(e == s.size());
    }
}