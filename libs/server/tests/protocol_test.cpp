// protocol（messages.h）单测：Message 序列化/反序列化、ChatRequest 字段往返、
// UTF-8 清理与安全 dump。

#include "messages.h"

#include <gtest/gtest.h>

#include <string>

using namespace codis;

namespace {

Message asst_tool_msg(const std::string& id, const std::string& name) {
    Message m;
    m.role = "assistant";
    m.tool_call_id = id;
    m.tool_name = name;
    m.tool_arguments = json{{"command", "ls"}};
    return m;
}

} // namespace

// 工具调用 assistant → OpenAI 标准 tool_calls 数组（content=null）
TEST(Protocol, ToolCallMessageToJson) {
    auto j = asst_tool_msg("call_1", "bash").to_json();
    EXPECT_EQ(j["role"], "assistant");
    EXPECT_TRUE(j["content"].is_null());
    ASSERT_TRUE(j["tool_calls"].is_array());
    ASSERT_EQ(j["tool_calls"].size(), 1u);
    EXPECT_EQ(j["tool_calls"][0]["id"], "call_1");
    EXPECT_EQ(j["tool_calls"][0]["type"], "function");
    EXPECT_EQ(j["tool_calls"][0]["function"]["name"], "bash");
    EXPECT_TRUE(j["tool_calls"][0]["function"]["arguments"].is_string());
    EXPECT_EQ(j["tool_calls"][0]["function"]["arguments"], "{\"command\":\"ls\"}");
}

// 标准 tool_calls 数组格式反序列化（content null 不抛；注意 from_json
// 只认平铺 tool_call_id/name/arguments 字段，tool_calls 数组不回读）
TEST(Protocol, ToolCallArrayFromJsonContentNull) {
    json j{
        {"role", "assistant"},
        {"content", nullptr},
        {"tool_calls", json::array({{
            {"id", "call_9"},
            {"type", "function"},
            {"function", {{"name", "grep"}, {"arguments", "{\"pattern\":\"x\"}"}}}
        }})}
    };
    auto m = Message::from_json(j);
    EXPECT_EQ(m.role, "assistant");
    EXPECT_TRUE(m.content.empty());
    EXPECT_FALSE(m.tool_call_id.has_value());
    EXPECT_FALSE(m.tool_name.has_value());
}

TEST(Protocol, ToolResultFlatFormatFromJson) {
    auto m = Message::from_json(json{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "out"}});
    EXPECT_EQ(m.role, "tool");
    EXPECT_EQ(m.content, "out");
    ASSERT_TRUE(m.tool_call_id.has_value());
    EXPECT_EQ(*m.tool_call_id, "call_1");
}

TEST(Protocol, MissingContentFromJson) {
    auto m = Message::from_json(json{{"role", "user"}});
    EXPECT_EQ(m.role, "user");
    EXPECT_TRUE(m.content.empty());
}

TEST(Protocol, ChatRequestSessionIdRoundtrip) {
    ChatRequest req;
    req.model = "glm-4.5-flash";
    req.session_id = "abc123";
    req.messages.push_back({"user", "hi"});
    auto r2 = ChatRequest::from_json(req.to_json());
    EXPECT_EQ(r2.session_id, "abc123");
    EXPECT_EQ(r2.model, "glm-4.5-flash");
    ASSERT_EQ(r2.messages.size(), 1u);
    EXPECT_EQ(r2.messages[0].content, "hi");
}

TEST(Protocol, ChatRequestDefaultsAndOmittedFields) {
    ChatRequest empty;
    auto e2 = ChatRequest::from_json(empty.to_json());
    EXPECT_TRUE(e2.session_id.empty());
    EXPECT_EQ(e2.model, "gpt-4o");       // 默认值
    EXPECT_TRUE(e2.messages.empty());
    EXPECT_FALSE(e2.max_tokens.has_value());
    EXPECT_FALSE(empty.to_json().contains("session_id"));
    EXPECT_FALSE(empty.to_json().contains("provider"));
}

TEST(Protocol, ChatRequestProviderTokensToolsRoundtrip) {
    ChatRequest req;
    req.provider = "deepseek";
    req.max_tokens = 2048;
    req.tools = json::array({{
        {"type", "function"},
        {"function", {{"name", "bash"}, {"parameters", json::object()}}}
    }});
    auto r2 = ChatRequest::from_json(req.to_json());
    EXPECT_EQ(r2.provider, "deepseek");
    ASSERT_TRUE(r2.max_tokens.has_value());
    EXPECT_EQ(*r2.max_tokens, 2048);
    EXPECT_EQ(r2.tools.size(), 1u);
}

TEST(Protocol, MakeValidUtf8) {
    EXPECT_EQ(make_valid_utf8("hello"), "hello");
    EXPECT_EQ(make_valid_utf8(""), "");
    EXPECT_EQ(make_valid_utf8(std::string("\xFF", 1)), "\xEF\xBF\xBD");
    EXPECT_EQ(make_valid_utf8(std::string("\xE4\xB8", 2)), "\xEF\xBF\xBD");   // "中" 截断
    EXPECT_EQ(make_valid_utf8(std::string("\xE4\xB8\xAD", 3)), "\xE4\xB8\xAD");  // 完整 "中"
    // overlong：非法前缀只吞首字节（续字节孤儿 → 各一个 U+FFFD）
    EXPECT_EQ(make_valid_utf8(std::string("\xC0\xAF", 2)), "\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST(Protocol, JsonDumpSafeReplacesInvalidUtf8) {
    json j{{"bad", std::string("\xFF\xFE", 2)}};
    auto s = json_dump_safe(j);
    EXPECT_NE(s.find("\xEF\xBF\xBD"), std::string::npos);
    EXPECT_FALSE(s.empty());
}