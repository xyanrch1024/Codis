// protocol（messages.h）单测：Message 序列化/反序列化、ChatRequest 字段往返、
// UTF-8 清理与安全 dump。

#include "messages.h"
#include "test_util.h"

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

void run_protocol_tests() {
    // 工具调用 assistant → OpenAI 标准 tool_calls 数组（content=null）
    {
        auto j = asst_tool_msg("call_1", "bash").to_json();
        CHECK(j["role"] == "assistant");
        CHECK(j["content"].is_null());
        CHECK(j["tool_calls"].is_array());
        CHECK(j["tool_calls"].size() == 1);
        CHECK(j["tool_calls"][0]["id"] == "call_1");
        CHECK(j["tool_calls"][0]["type"] == "function");
        CHECK(j["tool_calls"][0]["function"]["name"] == "bash");
        CHECK(j["tool_calls"][0]["function"]["arguments"].is_string());
        CHECK(j["tool_calls"][0]["function"]["arguments"] == "{\"command\":\"ls\"}");
    }

    // 标准 tool_calls 数组格式反序列化（content null 不抛；注意 from_json
    // 只认平铺 tool_call_id/name/arguments 字段，tool_calls 数组不回读）
    {
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
        CHECK(m.role == "assistant");
        CHECK(m.content.empty());
        CHECK(!m.tool_call_id.has_value());
        CHECK(!m.tool_name.has_value());
    }

    // content 缺失 / tool 结果消息（tool_call_id 平铺格式）
    {
        auto m = Message::from_json(json{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "out"}});
        CHECK(m.role == "tool");
        CHECK(m.content == "out");
        CHECK(*m.tool_call_id == "call_1");
    }
    {
        auto m = Message::from_json(json{{"role", "user"}});  // 无 content 字段
        CHECK(m.role == "user");
        CHECK(m.content.empty());
    }

    // ChatRequest：session_id 往返（新字段）与省略
    {
        ChatRequest req;
        req.model = "glm-4.5-flash";
        req.session_id = "abc123";
        req.messages.push_back({"user", "hi"});
        auto r2 = ChatRequest::from_json(req.to_json());
        CHECK(r2.session_id == "abc123");
        CHECK(r2.model == "glm-4.5-flash");
        CHECK(r2.messages.size() == 1);
        CHECK(r2.messages[0].content == "hi");

        ChatRequest empty;
        auto e2 = ChatRequest::from_json(empty.to_json());
        CHECK(e2.session_id.empty());
        CHECK(e2.model == "gpt-4o");       // 默认值
        CHECK(e2.messages.empty());
        CHECK(!e2.max_tokens.has_value());
        CHECK(!empty.to_json().contains("session_id"));
        CHECK(!empty.to_json().contains("provider"));
    }

    // ChatRequest：max_tokens / tools / provider 往返
    {
        ChatRequest req;
        req.provider = "deepseek";
        req.max_tokens = 2048;
        req.tools = json::array({{
            {"type", "function"},
            {"function", {{"name", "bash"}, {"parameters", json::object()}}}
        }});
        auto r2 = ChatRequest::from_json(req.to_json());
        CHECK(r2.provider == "deepseek");
        CHECK(*r2.max_tokens == 2048);
        CHECK(r2.tools.size() == 1);
    }

    // make_valid_utf8：ASCII 原样、非法字节 → U+FFFD、截断多字节 → U+FFFD
    {
        CHECK(make_valid_utf8("hello") == "hello");
        CHECK(make_valid_utf8("") == "");
        std::string bad("\xFF", 1);
        CHECK(make_valid_utf8(bad) == "\xEF\xBF\xBD");
        std::string cut("\xE4\xB8", 2);  // "中" 截断
        CHECK(make_valid_utf8(cut) == "\xEF\xBF\xBD");
        std::string tail("\xE4\xB8\xAD", 3);  // 完整 "中"
        CHECK(make_valid_utf8(tail) == "\xE4\xB8\xAD");
        // overlong：非法前缀只吞首字节（续字节孤儿 → 各一个 U+FFFD）
        CHECK(make_valid_utf8(std::string("\xC0\xAF", 2)) == "\xEF\xBF\xBD\xEF\xBF\xBD");
    }

    // json_dump_safe：非法 UTF-8 不抛异常
    {
        json j{{"bad", std::string("\xFF\xFE", 2)}};
        auto s = json_dump_safe(j);
        CHECK(s.find("\xEF\xBF\xBD") != std::string::npos);
        CHECK(s.size() > 0);
    }
}