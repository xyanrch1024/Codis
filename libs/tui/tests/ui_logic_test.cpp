// 纯逻辑单测：overlay 按键状态机、模型事件应用、命令分发（无终端/无网络）。
// 运行: ctest -R ui_logic  （或直接执行 codis_ui_test）

#include "controller.h"
#include "views.h"
#include "model.h"
#include "acp_client.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

#include <iostream>
#include <string>
#include <vector>

using namespace codis;
using ftxui::Event;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": " #cond \
                      << "\n";                                               \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// FakeAcpClient：内存替身，记录调用、供注入响应
// ---------------------------------------------------------------------------
class FakeAcpClient : public AcpClient {
public:
    FakeAcpClient() : AcpClient(9999) {}

    std::vector<ChatRequest> sent_requests;
    std::vector<std::string> http_paths;
    HttpResult http_result;
    int confirm_approved = -1;
    std::string last_switch;
    int create_calls = 0;
    bool return_info = false;
    ServerInfo server_info_;
    std::vector<SessionInfo> sessions;
    std::string last_compact_session;
    int last_compact_keep = -1;

    bool send_async(const ChatRequest& r) override {
        sent_requests.push_back(r);
        return true;
    }
    void send_confirmation(const std::string&, bool approved) override {
        confirm_approved = approved ? 1 : 0;
    }
    HttpResult http_get(const std::string& p) override {
        http_paths.push_back(p);
        return http_result;
    }
    std::optional<ServerInfo> get_server_info() override {
        if (return_info) return server_info_;
        return std::nullopt;
    }
    std::vector<SessionInfo> list_sessions() override { return sessions; }
    bool switch_session(const std::string& id) override {
        last_switch = id;
        return true;
    }
    std::optional<std::string> create_session() override {
        create_calls++;
        return "sess-new";
    }
    bool delete_session(const std::string&) override { return true; }
    std::optional<SessionInfo> get_session(const std::string& id) override {
        for (auto& s : sessions)
            if (s.id == id) return s;
        return std::nullopt;
    }
    void send_compact(const std::string& session, int keep) override {
        last_compact_session = session;
        last_compact_keep = keep;
    }
};

// ---------------------------------------------------------------------------
// Overlay 按键状态机
// ---------------------------------------------------------------------------
static void test_info_overlay() {
    InfoOverlay o;
    CHECK(!o.handle_key(Event::Escape));  // 不可见时不消费

    o.visible = true;
    o.skills = {{"skill-a", "A", "desc a"}, {"skill-b", "B", "desc b"}};
    o.mcps = {{"mcp-a", "stdio", true, 3}};

    CHECK(o.handle_key(Event::ArrowDown));
    CHECK(o.sel[0] == 1);
    CHECK(o.handle_key(Event::ArrowDown));  // 到底不越界
    CHECK(o.sel[0] == 1);
    CHECK(o.handle_key(Event::ArrowUp));
    CHECK(o.sel[0] == 0);
    CHECK(o.handle_key(Event::ArrowUp));    // 顶不再上
    CHECK(o.sel[0] == 0);

    CHECK(o.handle_key(Event::Tab));
    CHECK(o.pane == 1);
    CHECK(o.handle_key(Event::TabReverse));
    CHECK(o.pane == 0);
    CHECK(o.handle_key(Event::ArrowDown));  // mcp 栏 1 项不越界
    CHECK(o.sel[1] == 0);

    CHECK(o.handle_key(Event::Character('x')));  // 打开时吞掉普通键
    CHECK(o.handle_key(Event::Escape));
    CHECK(!o.visible);
}

static void test_sessions_overlay() {
    SessionsOverlay o;
    SessionInfo a, b;
    a.id = "aaa"; a.title = "first"; a.message_count = 1;
    b.id = "bbb"; b.title = "second"; b.message_count = 2;
    o.visible = true;
    o.list = {a, b};

    SessionInfo activated, deleted;
    o.on_activate = [&](const SessionInfo& s) { activated = s; };
    o.on_delete = [&](const SessionInfo& s) { deleted = s; };

    CHECK(o.handle_key(Event::ArrowUp));    // 顶处回绕？不——越界保护
    CHECK(o.selected == 0);
    CHECK(o.handle_key(Event::Tab));        // 回绕：0→1
    CHECK(o.selected == 1);
    CHECK(o.handle_key(Event::ArrowUp));
    CHECK(o.selected == 0);
    CHECK(o.handle_key(Event::ArrowDown));
    CHECK(o.selected == 1);
    CHECK(o.handle_key(Event::Return));
    CHECK(activated.id == "bbb");
    CHECK(o.handle_key(Event::d));
    CHECK(deleted.id == "bbb");
    CHECK(o.handle_key(Event::D));
    CHECK(deleted.id == "bbb");
    CHECK(o.handle_key(Event::Escape));
    CHECK(!o.visible);
    CHECK(!o.handle_key(Event::ArrowDown));  // 关闭后不消费
}

static void test_help_overlay() {
    HelpOverlay h;
    CHECK(!h.handle_key(Event::Escape));
    h.visible = true;
    CHECK(h.handle_key(Event::Character('q')));  // 吞掉
    CHECK(h.handle_key(Event::Escape));
    CHECK(!h.visible);
}

static void test_confirm_overlay() {
    ConfirmOverlay c;
    std::vector<bool> responses;
    c.on_respond = [&](bool approve) { responses.push_back(approve); };

    CHECK(c.handle_key(Event::Tab));       // 焦点 拒绝→批准
    CHECK(c.focus == true);
    CHECK(c.handle_key(Event::Return));    // Enter 激活焦点
    CHECK(responses.size() == 1 && responses[0] == true);
    CHECK(c.handle_key(Event::ArrowLeft)); // 焦点 批准→拒绝
    CHECK(c.focus == false);
    CHECK(c.handle_key(Event::Escape));    // 拒绝
    CHECK(responses.size() == 2 && responses[1] == false);
    CHECK(c.handle_key(Event::Character('y')));
    CHECK(responses.size() == 3 && responses[2] == true);
    CHECK(c.handle_key(Event::Character('n')));
    CHECK(responses.size() == 4 && responses[3] == false);
    CHECK(c.handle_key(Event::Character('x')));  // 其它键吞掉、无副作用
    CHECK(responses.size() == 4);
}

static void test_overlay_render_smoke() {
    TuiState st;
    auto layout = render_conversation(st, 60, -1);
    CHECK(layout.content != nullptr);
    CHECK(layout.row_owners.empty());

    ViewCtx ctx;
    ctx.state = &st;
    ctx.cwd = "/tmp";
    auto sb = render_status_bar(ctx);
    CHECK(sb != nullptr);

    HelpOverlay h;
    h.visible = true;
    CHECK(h.render(ftxui::text("x"), {}) != nullptr);
    InfoOverlay i;
    i.visible = true;
    CHECK(i.render(ftxui::text("x")) != nullptr);
    SessionsOverlay so;
    so.visible = true;
    so.list = {};
    CHECK(so.render(ftxui::text("x"), "sid") != nullptr);  // 空列表退回 body
    so.list = {{"sid", "t", 0, 0, 0, {}}};
    CHECK(so.render(ftxui::text("x"), "sid") != nullptr);
    ConfirmOverlay co;
    acp::ToolCallEvent call;
    call.name = "bash";
    call.arguments = json{{"command", "ls"}};
    CHECK(co.render(ftxui::text("x"), call, 30, 100) != nullptr);
    CHECK(co.height > 0);
}

// ---------------------------------------------------------------------------
// 模型事件应用（WS 事件 → 对话条目）
// ---------------------------------------------------------------------------
static void test_model_apply() {
    auto state = std::make_shared<TuiState>();
    AcpEvent ev;

    // 流式增量合并
    ev.kind = AcpEvent::Kind::AssistantDelta;
    ev.text = "hello";
    state->push_event(ev);
    ev.text = " world";
    state->push_event(ev);
    CHECK(state->drain_events());
    CHECK(state->items.size() == 1);
    CHECK(state->items[0].kind == ItemKind::Assistant);
    CHECK(state->items[0].text == "hello world");
    CHECK(state->items[0].streaming);

    // 工具调用 + 结果合并（按 id）
    acp::ToolCallEvent tc;
    tc.id = "c1";
    tc.name = "bash";
    tc.arguments = json{{"command", "ls"}};
    ev.kind = AcpEvent::Kind::ToolCall;
    ev.tool_call = tc;
    state->push_event(ev);

    acp::ToolResultEvent tr;
    tr.id = "c1";
    tr.success = true;
    tr.content = "file.txt";
    ev.kind = AcpEvent::Kind::ToolResult;
    ev.tool_result = tr;
    state->push_event(ev);

    CHECK(state->drain_events());
    CHECK(state->items.size() == 2);  // assistant + toolcall（空白行是渲染期概念）
    auto& tool_item = state->items.back();
    CHECK(tool_item.kind == ItemKind::ToolCall);
    CHECK(tool_item.has_result);
    CHECK(tool_item.tool_success);
    CHECK(tool_item.result_text == "file.txt");

    // Done：结束流式 + 停止 processing
    state->processing = true;
    state->current_model_ = "m";
    ev = AcpEvent{};
    ev.kind = AcpEvent::Kind::Done;
    state->push_event(ev);
    CHECK(state->drain_events());
    CHECK(!state->processing);
    CHECK(!state->items.back().streaming);

    // Error：入错误条目 + 停 processing
    ev = AcpEvent{};
    ev.kind = AcpEvent::Kind::Error;
    ev.text = "boom";
    state->push_event(ev);
    CHECK(state->drain_events());
    CHECK(state->items.back().kind == ItemKind::Error);
    CHECK(!state->processing);
}

// ---------------------------------------------------------------------------
// ChatController 命令分发
// ---------------------------------------------------------------------------
static void test_controller_basic() {
    FakeAcpClient acp;
    auto state = std::make_shared<TuiState>();
    state->current_session = "s1";
    ChatController ctrl(acp, state, "glm-4.5", "zhipu", false, 8711);

    int exit_calls = 0;
    bool sessions_shown = false;
    bool info_shown = false;
    bool help_shown = false;
    int hide_sessions_calls = 0;
    int reset_scroll_calls = 0;
    std::vector<std::string> notices;
    UiCallbacks cb;
    cb.exit = [&] { exit_calls++; };
    cb.show_sessions = [&](std::vector<SessionInfo>, bool) { sessions_shown = true; };
    cb.show_info = [&](std::vector<SkillBrief>, std::vector<McpServerBrief>) { info_shown = true; };
    cb.show_help = [&] { help_shown = true; };
    cb.hide_sessions = [&] { hide_sessions_calls++; };
    cb.reset_scroll = [&] { reset_scroll_calls++; };
    cb.notice = [&](const std::string& m) { notices.push_back(m); };
    ctrl.set_callbacks(std::move(cb));

    // /exit
    ctrl.send_message("/exit");
    CHECK(exit_calls == 1);

    // /sessions → show_sessions（list 来自 acp）
    acp.sessions = {{"sid1", "t", 0, 0, 0, {}}};
    ctrl.send_message("/sessions");
    CHECK(sessions_shown);

    // /help
    ctrl.send_message("/help");
    CHECK(help_shown);

    // /info（服务可达 → 传递 skills/mcps；不可达 → Error 条目 + 空列表）
    acp.return_info = true;
    acp.server_info_.skills = {{"s1", "n1", "d1"}};
    acp.server_info_.mcp_servers = {{"m1", "stdio", true, 2}};
    ctrl.send_message("/info");
    CHECK(info_shown);
    info_shown = false;
    acp.return_info = false;
    ctrl.send_message("/info");
    CHECK(info_shown);
    CHECK(state->items.back().kind == ItemKind::Error);

    // /yolo 切换 + notice
    ctrl.send_message("/yolo");
    CHECK(ctrl.yolo());
    CHECK(notices.back().find("ON") != std::string::npos);
    ctrl.send_message("/yolo off");
    CHECK(!ctrl.yolo());

    // 普通消息：User 条目 + send_async（仅当前一条）
    ctrl.send_message("hello world");
    CHECK(state->processing);
    CHECK(state->items.back().kind == ItemKind::User);
    CHECK(state->items.back().text == "hello world");
    CHECK(acp.sent_requests.size() == 1);
    CHECK(acp.sent_requests[0].session_id == "s1");
    CHECK(acp.sent_requests[0].messages[0].content == "hello world");
    CHECK(acp.sent_requests[0].max_tokens == 4096);

    // 任务中 → pending 队列，不发请求
    ctrl.send_message("queued msg");
    CHECK(acp.sent_requests.size() == 1);
    CHECK(state->pending_count() == 1);
    CHECK(reset_scroll_calls > 0);

    // Done 触发 on_idle_ → flush 逐条发送
    state->processing = false;
    state->on_idle_ = [&] { ctrl.flush_pending(); };
    state->on_idle_();
    CHECK(acp.sent_requests.size() == 2);
    CHECK(acp.sent_requests[1].messages[0].content == "queued msg");
}

static void test_controller_model_and_balance() {
    FakeAcpClient acp;
    auto state = std::make_shared<TuiState>();
    ChatController ctrl(acp, state, "glm-4.5", "zhipu", false, 8711);

    // /model <provider>
    acp.return_info = true;
    acp.server_info_.providers = {"zhipu", "deepseek"};
    acp.server_info_.provider_models = {{"zhipu", "glm-4.5"}, {"deepseek", "ds-v3"}};
    ctrl.send_message("/model deepseek");
    CHECK(ctrl.provider() == "deepseek");
    CHECK(ctrl.model() == "ds-v3");
    CHECK(state->model == "ds-v3");

    // 未知 provider 报错且不改状态
    ctrl.send_message("/model nope");
    CHECK(ctrl.provider() == "deepseek");
    CHECK(state->items.back().kind == ItemKind::Status);

    // /balance：走 http_get，成功解析出余额条目
    acp.http_result.ok = true;
    acp.http_result.status = 200;
    acp.http_result.body =
        R"({"balance": {"balance_infos": [{"total_balance": "12.34", "topped_up_balance": "10.00", "granted_balance": "2.34"}], "is_available": true}})";
    size_t before = state->items.size();
    ctrl.send_message("/balance deepseek");
    CHECK(acp.http_paths.size() == 1);
    CHECK(acp.http_paths[0] == "/api/v1/balance/deepseek");
    bool saw_total = false;
    for (size_t i = before; i < state->items.size(); i++)
        if (state->items[i].text.find("Total:") != std::string::npos) saw_total = true;
    CHECK(saw_total);

    // 网络失败 → 状态条目
    acp.http_result = HttpResult{};
    acp.http_result.ok = false;
    acp.http_result.error = "connection refused";
    ctrl.send_message("/balance");
    CHECK(state->items.back().text.find("unreachable") != std::string::npos);

    // 非 200 → 错误消息
    acp.http_result = HttpResult{};
    acp.http_result.ok = true;
    acp.http_result.status = 400;
    acp.http_result.body = R"({"error": "bad request"})";
    ctrl.send_message("/balance");
    CHECK(state->items.back().text.find("bad request") != std::string::npos);
}

static void test_controller_sessions() {
    FakeAcpClient acp;
    auto state = std::make_shared<TuiState>();
    state->current_session = "old";
    ChatController ctrl(acp, state, "m", "p", false, 8711);

    SessionInfo a, b;
    a.id = "old"; a.title = "t1";
    b.id = "new"; b.title = "t2";
    acp.sessions = {a, b};

    // switch_session：切换 current、拉历史、hide_sessions
    ctrl.switch_session(b);
    CHECK(state->current_session == "new");
    CHECK(acp.last_switch == "new");

    // delete_session：非当前 → 列表刷新，保留 overlay
    state->current_session = "new";
    ctrl.delete_session(a);
    CHECK(acp.sessions.size() == 2);

    // delete_session 当前 → 建新会话 + 清空
    ctrl.delete_session(b);
    CHECK(acp.create_calls >= 1);
    CHECK(state->current_session == "sess-new");
}

int main() {
    test_info_overlay();
    test_sessions_overlay();
    test_help_overlay();
    test_confirm_overlay();
    test_overlay_render_smoke();
    test_model_apply();
    test_controller_basic();
    test_controller_model_and_balance();
    test_controller_sessions();

    if (g_failures == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cerr << g_failures << " TEST(S) FAILED\n";
    return 1;
}