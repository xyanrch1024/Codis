// tool_calls 解析单测：markdown 块、文本前缀、截断 JSON 自动补括号、span 定位。

#include "context_utils.h"

#include <gtest/gtest.h>

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

TEST(ToolCallParse, NoToolCallsReturnsEmpty) {
    EXPECT_TRUE(extract_tool_calls("").empty());
    EXPECT_TRUE(extract_tool_calls("just text").empty());
    EXPECT_TRUE(extract_tool_calls("{}").empty());
}

TEST(ToolCallParse, MarkdownBlockSingleCall) {
    auto calls = extract_tool_calls(
        "```json\n{\"tool_calls\": [{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}}]}\n```");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].id, "call_1");
    EXPECT_EQ(calls[0].name, "bash");
    EXPECT_EQ(calls[0].arguments["command"], "ls");
}

TEST(ToolCallParse, MultipleCalls) {
    auto calls = extract_tool_calls(
        "{\"tool_calls\": ["
        "{\"id\":\"a\",\"function\":{\"name\":\"read\",\"arguments\":{\"path\":\"x\"}}},"
        "{\"id\":\"b\",\"function\":{\"name\":\"grep\",\"arguments\":{}}}]}");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(join_names(calls), "read:a,grep:b");
    EXPECT_TRUE(calls[1].arguments.empty());  // 缺 arguments → 空对象
}

TEST(ToolCallParse, MarkdownBlockWithSurroundingText) {
    auto calls = extract_tool_calls(
        "让我先看一下：\n```json\n{\"tool_calls\": [{\"id\":\"c1\","
        "\"function\":{\"name\":\"glob\",\"arguments\":{\"pattern\":\"**/*.cpp\"}}}]}\n```\n好的。");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].name, "glob");
}

TEST(ToolCallParse, PlainJsonWithTextPrefix) {
    auto calls = extract_tool_calls(
        "text before {\"tool_calls\": [{\"id\":\"x\","
        "\"function\":{\"name\":\"edit\",\"arguments\":{\"oldString\":\"a\",\"newString\":\"b\"}}}]}");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].name, "edit");
}

TEST(ToolCallParse, TruncatedMissingClosingBrace) {
    auto calls = extract_tool_calls(
        "{\"tool_calls\": [{\"id\":\"t1\","
        "\"function\":{\"name\":\"bash\",\"arguments\":{\"command\":\"ls\"}}}]");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].name, "bash");
}

TEST(ToolCallParse, TruncatedArrayMissingObject) {
    auto calls = extract_tool_calls(
        "{\"tool_calls\": [{\"id\":\"t2\","
        "\"function\":{\"name\":\"write\",\"arguments\":{\"path\":\"f\"}}}");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].name, "write");
}

// 字符串中途截断（如 "command":"g 后直接断流）：括号栈只能补括号、
// 补不了未闭合字符串 → 解析失败返回空（原实现行为，服务端走 malformed 重试路径）
TEST(ToolCallParse, TruncatedMidStringReturnsEmpty) {
    auto calls = extract_tool_calls(
        "{\"tool_calls\": [{\"id\":\"t3\","
        "\"function\":{\"name\":\"bash\",\"arguments\":{\"command\":\"g");
    EXPECT_TRUE(calls.empty());
}

TEST(ToolCallParse, InvalidJsonReturnsEmpty) {
    EXPECT_TRUE(extract_tool_calls("{\"tool_calls\": [oops").empty());
}

TEST(ToolCallJsonSpan, NotFound) {
    auto [b, e] = tool_calls_json_span("no calls here");
    EXPECT_EQ(b, std::string::npos);
    EXPECT_EQ(e, std::string::npos);
}

TEST(ToolCallJsonSpan, MarkdownFenceSpan) {
    std::string s = "pre\n```json\n{\"tool_calls\": []}\n```\npost";
    auto [b, e] = tool_calls_json_span(s);
    ASSERT_NE(b, std::string::npos);
    ASSERT_NE(e, std::string::npos);
    EXPECT_NE(s.substr(b, e - b).find("tool_calls"), std::string::npos);
    EXPECT_NE(s.substr(b, e - b).rfind("```"), std::string::npos);
}

TEST(ToolCallJsonSpan, TextPrefixSpanKeepsSurroundings) {
    std::string s = "text {\"tool_calls\": [{\"id\":\"1\",\"function\":{\"name\":\"n\",\"arguments\":{}}}]} tail";
    auto [b, e] = tool_calls_json_span(s);
    ASSERT_NE(b, std::string::npos);
    ASSERT_NE(e, std::string::npos);
    EXPECT_NE(s.substr(b, e - b).find("tool_calls"), std::string::npos);
    EXPECT_NE(s.substr(0, b).find("text"), std::string::npos);   // 文本前缀保留
    EXPECT_NE(s.substr(e).find("tail"), std::string::npos);      // 尾部文本保留
}

TEST(ToolCallJsonSpan, TruncatedSpansToEnd) {
    std::string s = "{\"tool_calls\": [{\"id\":\"1\",\"function\":{\"name\":\"n\"";
    auto [b, e] = tool_calls_json_span(s);
    ASSERT_NE(b, std::string::npos);
    EXPECT_EQ(e, s.size());
}

TEST(ToolCallJsonSpan, UnclosedMarkdownFenceSpansToEnd) {
    std::string s = "```json\n{\"tool_calls\": []";
    auto [b, e] = tool_calls_json_span(s);
    ASSERT_NE(b, std::string::npos);
    EXPECT_EQ(e, s.size());
}