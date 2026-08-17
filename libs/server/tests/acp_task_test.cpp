// AgentLoop 单测：turn 循环、权限、Ask 确认、重试、取消、压缩、广播语义。
// 全部同步执行（无网络）：FakeProvider 脚本化响应 + StubTool 记录执行。

#include "agent_loop.h"
#include "acp.h"
#include "context_utils.h"
#include "provider_registry.h"
#include "session_hub.h"
#include "session_store.h"
#include "tool_registry.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace codis;

namespace {

// =============================================================================
// 测试替身
// =============================================================================

struct ScriptItem {
    std::string content;                    // 经 on_token 逐段流式吐出
    std::string reasoning;                  // 经 on_reasoning 吐出
    bool success = true;
    LlmErrorCode error_code = LlmErrorCode::None;
    std::string error;
    bool set_cancel_flag = false;           // 流式期间置 abort 标志（模拟客户端取消）
    std::function<void()> before_stream;    // 每次 stream_chat 前调用
};

class FakeProvider : public LLMProvider {
public:
    std::string name() const override { return "fake"; }
    std::string get_model() const override { return "fake-model"; }

    void push(ScriptItem item) { script_.push_back(std::move(item)); }

    ChatResponse chat(const ChatRequest&) override {
        // 压缩摘要等非流式调用：消费脚本项，保留后一位调用者用的值
        calls_++;
        auto r = ChatResponse{};
        r.success = true;
        if (script_.empty()) {
            r.content = "chat-ok";
            return r;
        }
        auto item = std::move(script_.front());
        script_.pop_front();
        r.content = item.content;
        r.success = item.success;
        r.error_code = item.error_code;
        r.error = item.error;
        return r;
    }

    ChatResponse stream_chat(const ChatRequest&, TokenCallback on_token,
                             ReasoningCallback on_reasoning,
                             std::atomic<bool>* abort_flag) override {
        calls_++;
        if (script_.empty()) {
            auto r = ChatResponse{};
            r.success = true;
            return r;
        }
        auto item = std::move(script_.front());
        script_.pop_front();
        if (item.before_stream) item.before_stream();
        if (item.set_cancel_flag && abort_flag) abort_flag->store(true);
        if (!item.reasoning.empty() && on_reasoning) on_reasoning(item.reasoning);
        if (!item.content.empty() && on_token) {
            // 纯文本按 2 字节块流式（验证增量合并）；含 tool_calls 的内容把
            // JSON 部分整块吐出——服务端按 delta 子串抑制整块，模拟真实增量
            auto json_start = item.content.find('{');
            auto tc = item.content.find("tool_calls");
            if (tc == std::string::npos) {
                for (size_t i = 0; i < item.content.size(); i += 2)
                    on_token(std::string_view(item.content).substr(i, 2));
            } else if (json_start == std::string::npos || json_start > tc) {
                on_token(item.content);   // 找不到 JSON 起点，整块透传
            } else {
                for (size_t i = 0; i < json_start; i += 2)
                    on_token(std::string_view(item.content).substr(i, 2));
                on_token(std::string_view(item.content).substr(json_start));
            }
        }
        ChatResponse r;
        r.content = item.content;
        r.reasoning_content = item.reasoning;
        r.success = item.success;
        r.error_code = item.error_code;
        r.error = item.error;
        return r;
    }

    int calls() const { return calls_; }

private:
    std::deque<ScriptItem> script_;
    int calls_ = 0;
};

class StubTool : public Tool {
public:
    StubTool(std::string name, Permission perm = Permission::Allow,
             std::string out = "stub-ok")
        : name_(std::move(name)), perm_(perm), out_(std::move(out)) {}

    ToolSchema schema() const override {
        return {name_, "stub tool for tests", json::object()};
    }
    Permission default_permission() const override { return perm_; }
    ToolResult execute(const ToolCall& call) override {
        if (on_execute) on_execute();
        executions.push_back(call);
        return {call.id, true, out_};
    }

    std::vector<ToolCall> executions;
    std::function<void()> on_execute;  // 测试钩子：执行前触发（模拟执行期间外部事件）
    std::string name_;
    Permission perm_;
    std::string out_;
};

// =============================================================================
// 环境与工具函数
// =============================================================================

std::string sid = "sess-test-1";

struct TestEnv {
    AppConfig config;
    ProviderRegistry providers;
    ToolRegistry tools;
    SystemContext system;
    SessionStore store{":memory:"};
    SessionHub hub;
    AgentLoop loop;

    TestEnv() : loop(hub, store, system, tools, providers, config) {
        // 生产流程由 WS 建连时创建会话行；messages 外键引用 sessions，
        // 缺行时 INSERT 被 PRAGMA foreign_keys=ON 静默拒绝
        store.create_session_with_id(sid);
    }
};

ChatRequest user_req(const std::string& text = "hi") {
    ChatRequest req;
    req.session_id = sid;
    req.messages.push_back({"user", text});
    return req;
}

std::shared_ptr<FrameQueue> attach(TestEnv& env) {
    auto q = std::make_shared<FrameQueue>();
    env.hub.attach_connection(sid, "c1", q);
    return q;
}

// 同步任务后从队列取全部帧（try_pop 短超时轮询）
std::vector<acp::ParsedEvent> drain(std::shared_ptr<FrameQueue> q) {
    std::vector<acp::ParsedEvent> out;
    std::string raw;
    while (q->try_pop(raw, std::chrono::milliseconds(50))) {
        auto ev = acp::parse_frame(raw);
        if (!ev) {
            ADD_FAILURE() << "unparseable frame: " << raw;
            continue;
        }
        out.push_back(*ev);
    }
    return out;
}

std::vector<acp::ParsedEvent> filter(const std::vector<acp::ParsedEvent>& frames,
                                     acp::EventType type) {
    std::vector<acp::ParsedEvent> out;
    for (auto& f : frames)
        if (f.type == type) out.push_back(f);
    return out;
}

std::string join_text(const std::vector<acp::ParsedEvent>& frames) {
    std::string s;
    for (auto& f : frames)
        if (f.type == acp::EventType::assistant && f.data.contains("delta"))
            s += f.data["delta"].get<std::string>();
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// 基础流
// ---------------------------------------------------------------------------

TEST(AcpTask, SingleTurnPlainText) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content = "hello world"});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    EXPECT_EQ(join_text(frames), "hello world");
    EXPECT_EQ(filter(frames, acp::EventType::done).size(), 1u);
    EXPECT_TRUE(filter(frames, acp::EventType::error).empty());

    // 消息入库 + 会话回到 Idle（用户消息由 WS 层追加，此处直接传 req 不入库）
    auto msgs = env.store.load_messages(sid);
    ASSERT_GE(msgs.size(), 1u);
    EXPECT_EQ(msgs.back().role, "assistant");
    EXPECT_EQ(msgs.back().content, "hello world");
    EXPECT_TRUE(env.hub.start_task(sid, user_req()));  // Idle 可再次启动
}

TEST(AcpTask, ToolCallExecutedAndPersisted) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content =
        R"({"tool_calls": [{"id":"c1","type":"function","function":{"name":"stub","arguments":{"x":1}}}]})"});
    env.providers.register_custom("fake", fake);
    auto stub = std::make_unique<StubTool>("stub");
    auto* stub_ptr = stub.get();
    env.tools.register_tool(std::move(stub));

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    auto calls = filter(frames, acp::EventType::tool_call);
    auto results = filter(frames, acp::EventType::tool_result);
    ASSERT_EQ(calls.size(), 1u);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(calls[0].data["id"], "c1");
    EXPECT_EQ(results[0].data["success"], true);
    EXPECT_EQ(results[0].data["content"], "stub-ok");
    EXPECT_EQ(stub_ptr->executions.size(), 1u);
    // 纯 JSON 无正文 → assistant 文本条目不入库，tool 往返消息入库
    auto msgs = env.store.load_messages(sid);
    bool saw_tool = false, saw_asst = false;
    for (auto& m : msgs) {
        if (m.role == "tool" && m.tool_call_id == "c1") saw_tool = true;
        if (m.role == "assistant" && m.tool_call_id == "c1") saw_asst = true;
    }
    EXPECT_TRUE(saw_tool);
    EXPECT_TRUE(saw_asst);
}

TEST(AcpTask, ToolCallWithLeadingText) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content =
        "thinking...\n```json\n{\"tool_calls\": [{\"id\":\"c2\",\"function\":{\"name\":\"stub\",\"arguments\":{}}}]}\n```"});
    env.providers.register_custom("fake", fake);
    auto stub = std::make_unique<StubTool>("stub");
    auto* stub_ptr = stub.get();
    env.tools.register_tool(std::move(stub));

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    // 文本前缀逐块广播；JSON 块被服务端抑制（tool_calls 帧另行推送）
    EXPECT_EQ(join_text(frames), "thinking...\n```json\n");
    EXPECT_EQ(filter(frames, acp::EventType::tool_call).size(), 1u);
    auto msgs = env.store.load_messages(sid);
    EXPECT_EQ(msgs.back().role, "tool");
}

// ---------------------------------------------------------------------------
// 权限
// ---------------------------------------------------------------------------

TEST(AcpTask, PermissionDeniedSkipsExecution) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content =
        R"({"tool_calls": [{"id":"c1","function":{"name":"stub","arguments":{}}}]})"});
    env.providers.register_custom("fake", fake);
    auto stub = std::make_unique<StubTool>("stub");
    auto* stub_ptr = stub.get();
    env.tools.register_tool(std::move(stub));
    env.tools.set_permission("stub", Permission::Denied);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    auto results = filter(frames, acp::EventType::tool_result);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].data["success"], false);
    EXPECT_EQ(results[0].data["content"], "Permission denied");
    EXPECT_TRUE(stub_ptr->executions.empty());
}

// ---------------------------------------------------------------------------
// Ask 确认
// ---------------------------------------------------------------------------

namespace {
// 应答等待中的确认（approve=true 批准）。轮询等待 tool_confirm 帧出现。
void answer_confirm(TestEnv& env, std::shared_ptr<FrameQueue> q, bool approved,
                    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto frames = drain(q);
        for (auto& f : frames) {
            if (f.type != acp::EventType::tool_confirm) continue;
            std::string confirm_id = f.data.value("confirm_id", "");
            if (confirm_id.empty()) continue;
            auto s = env.hub.find_confirm(confirm_id);
            if (!s) continue;
            {
                std::lock_guard lk(s->mutex);
                s->approved = approved;
                s->answered = true;
            }
            s->cv.notify_one();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL() << "timed out waiting for tool_confirm frame";
}
} // namespace

TEST(AcpTask, AskApprovedExecutes) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content =
        R"({"tool_calls": [{"id":"c1","function":{"name":"stub","arguments":{}}}]})"});
    env.providers.register_custom("fake", fake);
    auto stub = std::make_unique<StubTool>("stub", Permission::Ask);
    auto* stub_ptr = stub.get();
    env.tools.register_tool(std::move(stub));

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    std::thread runner([&] { env.loop.run_task(sid, "c1", user_req()); });
    answer_confirm(env, q, true);
    runner.join();

    auto frames = drain(q);
    auto results = filter(frames, acp::EventType::tool_result);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].data["success"], true);
    EXPECT_EQ(stub_ptr->executions.size(), 1u);
}

TEST(AcpTask, AskRejectedSkipsExecution) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content =
        R"({"tool_calls": [{"id":"c1","function":{"name":"stub","arguments":{}}}]})"});
    env.providers.register_custom("fake", fake);
    auto stub = std::make_unique<StubTool>("stub", Permission::Ask);
    auto* stub_ptr = stub.get();
    env.tools.register_tool(std::move(stub));

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    std::thread runner([&] { env.loop.run_task(sid, "c1", user_req()); });
    answer_confirm(env, q, false);
    runner.join();

    auto frames = drain(q);
    auto results = filter(frames, acp::EventType::tool_result);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].data["success"], false);
    EXPECT_NE(std::string(results[0].data["content"]).find("rejected"), std::string::npos);
    EXPECT_TRUE(stub_ptr->executions.empty());
}

// ---------------------------------------------------------------------------
// 重试
// ---------------------------------------------------------------------------

TEST(AcpTask, EmptyResponseRetriesThenSucceeds) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{});                    // 空响应
    fake->push(ScriptItem{.content = "ok"});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    EXPECT_EQ(join_text(frames), "ok");
    EXPECT_TRUE(filter(frames, acp::EventType::error).empty());
    EXPECT_EQ(fake->calls(), 2);
}

TEST(AcpTask, EmptyResponseExhaustedReportsError) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{});
    fake->push(ScriptItem{});
    fake->push(ScriptItem{});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    auto errors = filter(frames, acp::EventType::error);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(std::string(errors[0].data["message"]).find("空响应"), std::string::npos);
}

TEST(AcpTask, MalformedToolCallsRetriesThenSucceeds) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content = R"({"tool_calls": [oops)"});   // 含 tool_calls 标记但解析失败
    fake->push(ScriptItem{.content = "fixed"});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    EXPECT_EQ(join_text(frames), "fixed");
    EXPECT_TRUE(filter(frames, acp::EventType::error).empty());
    EXPECT_EQ(fake->calls(), 2);
}

TEST(AcpTask, MalformedToolCallsExhaustedReportsError) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    // 4 次：前 3 次触发重试，第 4 次超限 → 报错（脚本耗尽后的空响应不会劫持该分支）
    for (int i = 0; i < 4; i++) fake->push(ScriptItem{.content = R"({"tool_calls": [oops)"});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    auto errors = filter(frames, acp::EventType::error);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(std::string(errors[0].data["message"]).find("格式错误"), std::string::npos);
    EXPECT_EQ(fake->calls(), 4);
}

TEST(AcpTask, LlmFailureBroadcastsError) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.success = false, .error = "timeout"});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    auto errors = filter(frames, acp::EventType::error);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(std::string(errors[0].data["message"]).find("timeout"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 取消与 turn 上限
// ---------------------------------------------------------------------------

TEST(AcpTask, ClientCancelDuringStreamStops) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content = "partial", .set_cancel_flag = true});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    // 取消路径原行为：取消检查处广播 done + 循环结束广播 done（共 2 帧）
    EXPECT_GE(filter(frames, acp::EventType::done).size(), 1u);
    EXPECT_TRUE(filter(frames, acp::EventType::tool_call).empty());
    EXPECT_TRUE(filter(frames, acp::EventType::error).empty());
    // 取消不落库
    auto msgs = env.store.load_messages(sid);
    for (auto& m : msgs) EXPECT_NE(m.role, "assistant");
}

TEST(AcpTask, CancelAtLoopTopBroadcastsCanceledError) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content =
        R"({"tool_calls": [{"id":"c1","function":{"name":"stub","arguments":{}}}]})"});
    env.providers.register_custom("fake", fake);
    auto stub = std::make_unique<StubTool>("stub");
    auto* stub_ptr = stub.get();
    // 首轮工具执行期间客户端发来 cancel → 第二轮循环顶部广播 canceled
    stub->on_execute = [&env] { env.hub.request_cancel(sid); };
    env.tools.register_tool(std::move(stub));

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    auto errors = filter(frames, acp::EventType::error);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors[0].data["message"], "canceled");
    EXPECT_EQ(stub_ptr->executions.size(), 1u);   // 首轮工具已执行
}

TEST(AcpTask, MaxTurnsReached) {
    TestEnv env;
    auto q = attach(env);
    env.loop.set_max_turns(3);
    auto fake = std::make_shared<FakeProvider>();
    for (int i = 0; i < 5; i++) fake->push(ScriptItem{.content =
        R"({"tool_calls": [{"id":"c1","function":{"name":"stub","arguments":{}}}]})"});
    env.providers.register_custom("fake", fake);
    auto stub = std::make_unique<StubTool>("stub");
    auto* stub_ptr = stub.get();
    env.tools.register_tool(std::move(stub));

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());

    auto frames = drain(q);
    auto errors = filter(frames, acp::EventType::error);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(std::string(errors[0].data["message"]).find("Max turns"), std::string::npos);
    EXPECT_EQ(stub_ptr->executions.size(), 3u);
    EXPECT_EQ(fake->calls(), 3);
}

// ---------------------------------------------------------------------------
// 广播语义
// ---------------------------------------------------------------------------

TEST(AcpTask, BroadcastDirectedToConn) {
    TestEnv env;
    auto q1 = std::make_shared<FrameQueue>();
    auto q2 = std::make_shared<FrameQueue>();
    env.hub.attach_connection(sid, "c1", q1);
    env.hub.attach_connection(sid, "c2", q2);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content = "hi"});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "c1", user_req());   // 定向 c1

    auto f1 = drain(q1);
    auto f2 = drain(q2);
    EXPECT_EQ(join_text(f1), "hi");
    EXPECT_TRUE(f2.empty());                     // c2 不收到

    fake->push(ScriptItem{.content = "again"});
    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "", user_req());      // 空 conn_id → 全会话

    auto g1 = drain(q1);
    auto g2 = drain(q2);
    EXPECT_EQ(join_text(g1), "again");
    EXPECT_EQ(join_text(g2), "again");
}

TEST(AcpTask, BroadcastMissingConnDoesNotThrow) {
    TestEnv env;
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content = "hi"});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    env.loop.run_task(sid, "ghost-conn", user_req());  // 不存在的 conn 不崩
    // 广播 drop 打 WARN；无帧可验证，任务正常收尾即可
    EXPECT_TRUE(env.hub.start_task(sid, user_req()));
}

// ---------------------------------------------------------------------------
// 上下文压缩
// ---------------------------------------------------------------------------

TEST(AcpTask, CompactShortHistoryFailsAndRecovers) {
    TestEnv env;
    auto q = attach(env);
    env.store.create_session_with_id(sid);
    env.store.append_message(sid, {"user", "only one msg"});

    auto next = env.loop.run_compact(sid, 4);
    EXPECT_FALSE(next.has_value());

    auto frames = drain(q);
    auto compacted = filter(frames, acp::EventType::compacted);
    ASSERT_EQ(compacted.size(), 1u);
    EXPECT_EQ(compacted[0].data["ok"], false);
    // 回归：短历史 fail 后会话必须恢复 Idle（原实现 processing 残留导致永久卡死）
    EXPECT_TRUE(env.hub.start_task(sid, user_req()));
}

TEST(AcpTask, CompactLongHistoryReplacesStore) {
    TestEnv env;
    auto q = attach(env);
    env.store.create_session_with_id(sid);
    for (int i = 0; i < 10; i++)
        env.store.append_message(sid, {"user", "u" + std::to_string(i)});
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content = "THE SUMMARY"});
    env.providers.register_custom("fake", fake);

    auto next = env.loop.run_compact(sid, 4);
    EXPECT_FALSE(next.has_value());

    auto frames = drain(q);
    auto compacted = filter(frames, acp::EventType::compacted);
    ASSERT_EQ(compacted.size(), 1u);
    EXPECT_EQ(compacted[0].data["ok"], true);
    EXPECT_EQ(compacted[0].data["summary"], "THE SUMMARY");

    auto msgs = env.store.load_messages(sid);
    ASSERT_EQ(msgs.size(), 1u + 1u + 4u);   // system 摘要 + 题干 + 尾部 4
    EXPECT_EQ(msgs[0].role, "system");
    EXPECT_NE(msgs[0].content.find("THE SUMMARY"), std::string::npos);
    EXPECT_EQ(msgs[1].content, "u0");
    EXPECT_EQ(msgs[2].content, "u6");
    EXPECT_EQ(msgs.back().content, "u9");
    EXPECT_EQ(fake->calls(), 1);
}

TEST(AcpTask, CompactRejectedWhileTaskRunning) {
    TestEnv env;
    auto q = attach(env);
    auto fake = std::make_shared<FakeProvider>();
    fake->push(ScriptItem{.content = "hi"});
    env.providers.register_custom("fake", fake);

    ASSERT_TRUE(env.hub.start_task(sid, user_req()));
    // 任务进行中（未结束）→ compact 拒绝；run_compact 返回 nullopt 且不改相位
    auto next = env.loop.run_compact(sid, 4);
    EXPECT_FALSE(next.has_value());
    env.loop.run_task(sid, "c1", user_req());   // 任务正常收尾
    auto frames = drain(q);
    auto compacted = filter(frames, acp::EventType::compacted);
    ASSERT_EQ(compacted.size(), 1u);
    EXPECT_EQ(compacted[0].data["ok"], false);
}

// ---------------------------------------------------------------------------
// SessionHub 状态机与连接管理
// ---------------------------------------------------------------------------

TEST(SessionHubState, PendingQueuedAndDrained) {
    TestEnv env;
    // 任务进行中 → 第二个请求入队
    ASSERT_TRUE(env.hub.start_task(sid, user_req("first")));
    EXPECT_FALSE(env.hub.start_task(sid, user_req("second")));
    EXPECT_FALSE(env.hub.start_task(sid, user_req("third")));

    auto next = env.hub.next_task_or_idle(sid);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->messages.back().content, "second");
    // 队列中还有 → 保持 Running，不回 Idle
    EXPECT_FALSE(env.hub.start_task(sid, user_req("x")));

    next = env.hub.next_task_or_idle(sid);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->messages.back().content, "third");
    // 647 排队的消息也按序补跑
    next = env.hub.next_task_or_idle(sid);
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->messages.back().content, "x");
    // 队列空 → 回 Idle
    EXPECT_FALSE(env.hub.next_task_or_idle(sid).has_value());
    EXPECT_TRUE(env.hub.start_task(sid, user_req("fresh")));
}

TEST(SessionHubState, RequestCancelClearsPendingAndConfirms) {
    TestEnv env;
    ASSERT_TRUE(env.hub.start_task(sid, user_req("first")));
    EXPECT_FALSE(env.hub.start_task(sid, user_req("second")));
    env.hub.add_confirm(sid, "cf1");

    env.hub.request_cancel(sid);

    EXPECT_TRUE(env.hub.cancel_token(sid)->load());
    EXPECT_FALSE(env.hub.next_task_or_idle(sid).has_value());  // pending 已清空 → Idle
    auto slot = env.hub.find_confirm("cf1");
    ASSERT_TRUE(slot != nullptr);
    {
        std::lock_guard lk(slot->mutex);
        EXPECT_TRUE(slot->answered);
        EXPECT_FALSE(slot->approved);
    }
}

TEST(SessionHubState, MoveConnectionTransfersQueue) {
    TestEnv env;
    auto q = std::make_shared<FrameQueue>();
    env.hub.attach_connection("sess-a", "c1", q);
    auto moved = env.hub.move_connection("c1", "sess-b");
    ASSERT_TRUE(moved != nullptr);
    EXPECT_EQ(moved, q);

    // 旧会话无连接 → 条目清除；广播到旧会话无帧
    env.hub.broadcast("sess-a", "", "x");
    EXPECT_TRUE(env.hub.move_connection("nope", "sess-c") == nullptr);  // 不存在的 conn
    env.hub.broadcast("sess-b", "c1", "ping");
    std::string raw;
    ASSERT_TRUE(q->try_pop(raw, std::chrono::milliseconds(200)));
    EXPECT_EQ(raw, "ping");
}

TEST(SessionHubState, DetachRemovesAliasedConnections) {
    TestEnv env;
    auto q = std::make_shared<FrameQueue>();
    env.hub.attach_connection(sid, "c1", q);
    env.hub.migrate_reconnect("c1", sid, q);   // 别名键 old=c1 → 同一队列
    env.hub.detach_connection(sid, q);          // 按队列值清理全部别名
    env.hub.broadcast(sid, "", "x");            // 无连接 → 静默
    std::string raw;
    EXPECT_FALSE(q->try_pop(raw, std::chrono::milliseconds(100)));
}