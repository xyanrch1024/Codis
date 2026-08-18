// 纯逻辑单测：overlay 按键状态机、模型事件应用、命令分发（无终端/无网络）。
// 运行: ctest -R ui_logic  （或直接执行 codis_ui_test）

#include "controller.h"
#include "views.h"
#include "model.h"
#include "acp_client.h"
#include "tui_tool_render.h"

#include <gtest/gtest.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

#include <string>
#include <vector>

using namespace codis;
using ftxui::Event;

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
TEST(UiOverlay, InfoOverlayKeys) {
    InfoOverlay o;
    EXPECT_FALSE(o.handle_key(Event::Escape));  // 不可见时不消费

    o.visible = true;
    o.skills = {{"skill-a", "A", "desc a"}, {"skill-b", "B", "desc b"}};
    o.mcps = {{"mcp-a", "stdio", true, 3}};

    EXPECT_TRUE(o.handle_key(Event::ArrowDown));
    EXPECT_EQ(o.sel[0], 1);
    EXPECT_TRUE(o.handle_key(Event::ArrowDown));  // 到底不越界
    EXPECT_EQ(o.sel[0], 1);
    EXPECT_TRUE(o.handle_key(Event::ArrowUp));
    EXPECT_EQ(o.sel[0], 0);
    EXPECT_TRUE(o.handle_key(Event::ArrowUp));    // 顶不再上
    EXPECT_EQ(o.sel[0], 0);

    EXPECT_TRUE(o.handle_key(Event::Tab));
    EXPECT_EQ(o.pane, 1);
    EXPECT_TRUE(o.handle_key(Event::TabReverse));
    EXPECT_EQ(o.pane, 0);
    EXPECT_TRUE(o.handle_key(Event::ArrowDown));  // mcp 栏 1 项不越界
    EXPECT_EQ(o.sel[1], 0);

    EXPECT_TRUE(o.handle_key(Event::Character('x')));  // 打开时吞掉普通键
    EXPECT_TRUE(o.handle_key(Event::Escape));
    EXPECT_FALSE(o.visible);
}

TEST(UiOverlay, SessionsOverlayKeys) {
    SessionsOverlay o;
    SessionInfo a, b;
    a.id = "aaa"; a.title = "first"; a.message_count = 1;
    b.id = "bbb"; b.title = "second"; b.message_count = 2;
    o.visible = true;
    o.list = {a, b};

    SessionInfo activated, deleted;
    o.on_activate = [&](const SessionInfo& s) { activated = s; };
    o.on_delete = [&](const SessionInfo& s) { deleted = s; };

    EXPECT_TRUE(o.handle_key(Event::ArrowUp));    // 顶处回绕？不——越界保护
    EXPECT_EQ(o.selected, 0);
    EXPECT_TRUE(o.handle_key(Event::Tab));        // 回绕：0→1
    EXPECT_EQ(o.selected, 1);
    EXPECT_TRUE(o.handle_key(Event::ArrowUp));
    EXPECT_EQ(o.selected, 0);
    EXPECT_TRUE(o.handle_key(Event::ArrowDown));
    EXPECT_EQ(o.selected, 1);
    EXPECT_TRUE(o.handle_key(Event::Return));
    EXPECT_EQ(activated.id, "bbb");
    EXPECT_TRUE(o.handle_key(Event::d));
    EXPECT_EQ(deleted.id, "bbb");
    EXPECT_TRUE(o.handle_key(Event::D));
    EXPECT_EQ(deleted.id, "bbb");
    EXPECT_TRUE(o.handle_key(Event::Escape));
    EXPECT_FALSE(o.visible);
    EXPECT_FALSE(o.handle_key(Event::ArrowDown));  // 关闭后不消费
}

TEST(UiOverlay, ModelPickerKeys) {
    ModelOverlay o;
    EXPECT_FALSE(o.handle_key(Event::Escape));  // 不可见时不消费

    o.visible = true;
    o.list = {{"glm", "glm-4.5-flash"}, {"deepseek", "deepseek-v3"}};

    std::string activated;
    o.on_activate = [&](const std::string& p) { activated = p; };

    // Tab 循环：0→1→0
    EXPECT_TRUE(o.handle_key(Event::Tab));
    EXPECT_EQ(o.selected, 1);
    EXPECT_TRUE(o.handle_key(Event::Tab));
    EXPECT_EQ(o.selected, 0);
    // TabReverse / ↑↓
    EXPECT_TRUE(o.handle_key(Event::ArrowDown));
    EXPECT_EQ(o.selected, 1);
    EXPECT_TRUE(o.handle_key(Event::ArrowUp));
    EXPECT_EQ(o.selected, 0);
    EXPECT_TRUE(o.handle_key(Event::TabReverse));  // 0 回绕到末尾
    EXPECT_EQ(o.selected, 1);

    // Enter 应用所选 provider
    EXPECT_TRUE(o.handle_key(Event::Return));
    EXPECT_EQ(activated, "deepseek");
    EXPECT_TRUE(o.visible);  // 应用后面板保持打开（可继续试其它模型）

    // ESC 关闭
    EXPECT_TRUE(o.handle_key(Event::Character('x')));  // 打开时吞掉普通键
    EXPECT_TRUE(o.handle_key(Event::Escape));
    EXPECT_FALSE(o.visible);

    // 空列表：无选中索引，ESC 可关，其余吞掉
    o.visible = true;
    o.list.clear();
    EXPECT_TRUE(o.handle_key(Event::Tab));
    EXPECT_TRUE(o.handle_key(Event::Return));
    EXPECT_TRUE(o.handle_key(Event::Escape));
    EXPECT_FALSE(o.visible);
}

TEST(UiOverlay, HelpOverlayKeys) {
    HelpOverlay h;
    EXPECT_FALSE(h.handle_key(Event::Escape));
    h.visible = true;
    EXPECT_TRUE(h.handle_key(Event::Character('q')));  // 吞掉
    EXPECT_TRUE(h.handle_key(Event::Escape));
    EXPECT_FALSE(h.visible);
}

TEST(UiOverlay, ConfirmOverlayKeys) {
    ConfirmOverlay c;
    std::vector<bool> responses;
    c.on_respond = [&](bool approve) { responses.push_back(approve); };

    EXPECT_TRUE(c.handle_key(Event::Tab));       // 焦点 拒绝→批准
    EXPECT_TRUE(c.focus);
    EXPECT_TRUE(c.handle_key(Event::Return));    // Enter 激活焦点
    ASSERT_EQ(responses.size(), 1u);
    EXPECT_TRUE(responses[0]);
    EXPECT_TRUE(c.handle_key(Event::ArrowLeft)); // 焦点 批准→拒绝
    EXPECT_FALSE(c.focus);
    EXPECT_TRUE(c.handle_key(Event::Escape));    // 拒绝
    ASSERT_EQ(responses.size(), 2u);
    EXPECT_FALSE(responses[1]);
    EXPECT_TRUE(c.handle_key(Event::Character('y')));
    ASSERT_EQ(responses.size(), 3u);
    EXPECT_TRUE(responses[2]);
    EXPECT_TRUE(c.handle_key(Event::Character('n')));
    ASSERT_EQ(responses.size(), 4u);
    EXPECT_FALSE(responses[3]);
    EXPECT_TRUE(c.handle_key(Event::Character('x')));  // 其它键吞掉、无副作用
    EXPECT_EQ(responses.size(), 4u);
}

TEST(UiOverlay, RenderSmoke) {
    TuiState st;
    auto layout = render_conversation(st, 60, -1);
    EXPECT_NE(layout.content, nullptr);
    EXPECT_TRUE(layout.row_owners.empty());

    ViewCtx ctx;
    ctx.state = &st;
    ctx.cwd = "/tmp";
    auto sb = render_status_bar(ctx);
    EXPECT_NE(sb, nullptr);

    HelpOverlay h;
    h.visible = true;
    EXPECT_NE(h.render(ftxui::text("x"), {}), nullptr);
    InfoOverlay i;
    i.visible = true;
    EXPECT_NE(i.render(ftxui::text("x")), nullptr);
    SessionsOverlay so;
    so.visible = true;
    so.list = {};
    EXPECT_NE(so.render(ftxui::text("x"), "sid"), nullptr);  // 空列表退回 body
    so.list = {{"sid", "t", 0, 0, 0, {}}};
    EXPECT_NE(so.render(ftxui::text("x"), "sid"), nullptr);
    ConfirmOverlay co;
    acp::ToolCallEvent call;
    call.name = "bash";
    call.arguments = json{{"command", "ls"}};
    EXPECT_NE(co.render(ftxui::text("x"), call, 30, 100), nullptr);
    EXPECT_GT(co.height, 0);
}

// ---------------------------------------------------------------------------
// 模型事件应用（WS 事件 → 对话条目）
// ---------------------------------------------------------------------------
TEST(UiModel, ApplyEvents) {
    auto state = std::make_shared<TuiState>();
    AcpEvent ev;

    // 流式增量合并
    ev.kind = AcpEvent::Kind::AssistantDelta;
    ev.text = "hello";
    state->push_event(ev);
    ev.text = " world";
    state->push_event(ev);
    EXPECT_TRUE(state->drain_events());
    ASSERT_EQ(state->items.size(), 1u);
    EXPECT_EQ(state->items[0].kind, ItemKind::Assistant);
    EXPECT_EQ(state->items[0].text, "hello world");
    EXPECT_TRUE(state->items[0].streaming);

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

    EXPECT_TRUE(state->drain_events());
    ASSERT_EQ(state->items.size(), 2u);  // assistant + toolcall（空白行是渲染期概念）
    auto& tool_item = state->items.back();
    EXPECT_EQ(tool_item.kind, ItemKind::ToolCall);
    EXPECT_TRUE(tool_item.has_result);
    EXPECT_TRUE(tool_item.tool_success);
    EXPECT_EQ(tool_item.result_text, "file.txt");

    // Done：结束流式 + 停止 processing
    state->processing = true;
    state->current_model_ = "m";
    ev = AcpEvent{};
    ev.kind = AcpEvent::Kind::Done;
    state->push_event(ev);
    EXPECT_TRUE(state->drain_events());
    EXPECT_FALSE(state->processing);
    EXPECT_FALSE(state->items.back().streaming);

    // Error：入错误条目 + 停 processing
    ev = AcpEvent{};
    ev.kind = AcpEvent::Kind::Error;
    ev.text = "boom";
    state->push_event(ev);
    EXPECT_TRUE(state->drain_events());
    EXPECT_EQ(state->items.back().kind, ItemKind::Error);
    EXPECT_FALSE(state->processing);
}

// ---------------------------------------------------------------------------
// ChatController 命令分发
// ---------------------------------------------------------------------------
TEST(UiController, BasicCommands) {
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
    EXPECT_EQ(exit_calls, 1);

    // /sessions → show_sessions（list 来自 acp）
    acp.sessions = {{"sid1", "t", 0, 0, 0, {}}};
    ctrl.send_message("/sessions");
    EXPECT_TRUE(sessions_shown);

    // /help
    ctrl.send_message("/help");
    EXPECT_TRUE(help_shown);

    // /info（服务可达 → 传递 skills/mcps；不可达 → Error 条目 + 空列表）
    acp.return_info = true;
    acp.server_info_.skills = {{"s1", "n1", "d1"}};
    acp.server_info_.mcp_servers = {{"m1", "stdio", true, 2}};
    ctrl.send_message("/info");
    EXPECT_TRUE(info_shown);
    info_shown = false;
    acp.return_info = false;
    ctrl.send_message("/info");
    EXPECT_TRUE(info_shown);
    EXPECT_EQ(state->items.back().kind, ItemKind::Error);

    // /yolo 切换 + notice
    ctrl.send_message("/yolo");
    EXPECT_TRUE(ctrl.yolo());
    EXPECT_NE(notices.back().find("ON"), std::string::npos);
    ctrl.send_message("/yolo off");
    EXPECT_FALSE(ctrl.yolo());

    // 普通消息：User 条目 + send_async（仅当前一条）
    ctrl.send_message("hello world");
    EXPECT_TRUE(state->processing);
    EXPECT_EQ(state->items.back().kind, ItemKind::User);
    EXPECT_EQ(state->items.back().text, "hello world");
    ASSERT_EQ(acp.sent_requests.size(), 1u);
    EXPECT_EQ(acp.sent_requests[0].session_id, "s1");
    EXPECT_EQ(acp.sent_requests[0].messages[0].content, "hello world");
    EXPECT_EQ(acp.sent_requests[0].max_tokens, 4096);

    // 任务中 → pending 队列，不发请求
    ctrl.send_message("queued msg");
    EXPECT_EQ(acp.sent_requests.size(), 1u);
    EXPECT_EQ(state->pending_count(), 1);
    EXPECT_GT(reset_scroll_calls, 0);

    // Done 触发 on_idle_ → flush 逐条发送
    state->processing = false;
    state->on_idle_ = [&] { ctrl.flush_pending(); };
    state->on_idle_();
    ASSERT_EQ(acp.sent_requests.size(), 2u);
    EXPECT_EQ(acp.sent_requests[1].messages[0].content, "queued msg");
}

TEST(UiController, ModelAndBalanceCommands) {
    FakeAcpClient acp;
    auto state = std::make_shared<TuiState>();
    ChatController ctrl(acp, state, "glm-4.5", "zhipu", false, 8711);

    // /model <provider>
    acp.return_info = true;
    acp.server_info_.providers = {"zhipu", "deepseek"};
    acp.server_info_.provider_models = {{"zhipu", "glm-4.5"}, {"deepseek", "ds-v3"}};
    ctrl.send_message("/model deepseek");
    EXPECT_EQ(ctrl.provider(), "deepseek");
    EXPECT_EQ(ctrl.model(), "ds-v3");
    EXPECT_EQ(state->model, "ds-v3");

    // 未知 provider 报错且不改状态
    ctrl.send_message("/model nope");
    EXPECT_EQ(ctrl.provider(), "deepseek");
    EXPECT_EQ(state->items.back().kind, ItemKind::Status);

    // /model（无参）→ 打开模型下拉选择面板（列表 = providers + 模型名）
    std::vector<std::pair<std::string, std::string>> picker_list;
    bool picker_shown = false;
    UiCallbacks cb;
    cb.show_model_picker = [&](std::vector<std::pair<std::string, std::string>> list, bool) {
        picker_shown = true;
        picker_list = std::move(list);
    };
    ctrl.set_callbacks(std::move(cb));
    ctrl.send_message("/model");
    EXPECT_TRUE(picker_shown);
    ASSERT_EQ(picker_list.size(), 2u);
    EXPECT_EQ(picker_list[0].first, "zhipu");
    EXPECT_EQ(picker_list[0].second, "glm-4.5");
    EXPECT_EQ(picker_list[1].first, "deepseek");
    EXPECT_EQ(picker_list[1].second, "ds-v3");

    // 服务不可达 → 错误状态条目，不开面板
    picker_shown = false;
    acp.return_info = false;
    ctrl.send_message("/model");
    EXPECT_FALSE(picker_shown);
    EXPECT_EQ(state->items.back().kind, ItemKind::Status);
    EXPECT_NE(state->items.back().text.find("unreachable"), std::string::npos);

    // /balance：走 http_get，成功解析出余额条目
    acp.http_result.ok = true;
    acp.http_result.status = 200;
    acp.http_result.body =
        R"({"balance": {"balance_infos": [{"total_balance": "12.34", "topped_up_balance": "10.00", "granted_balance": "2.34"}], "is_available": true}})";
    size_t before = state->items.size();
    ctrl.send_message("/balance deepseek");
    ASSERT_EQ(acp.http_paths.size(), 1u);
    EXPECT_EQ(acp.http_paths[0], "/api/v1/balance/deepseek");
    bool saw_total = false;
    for (size_t i = before; i < state->items.size(); i++)
        if (state->items[i].text.find("Total:") != std::string::npos) saw_total = true;
    EXPECT_TRUE(saw_total);

    // 网络失败 → 状态条目
    acp.http_result = HttpResult{};
    acp.http_result.ok = false;
    acp.http_result.error = "connection refused";
    ctrl.send_message("/balance");
    EXPECT_NE(state->items.back().text.find("unreachable"), std::string::npos);

    // 非 200 → 错误消息
    acp.http_result = HttpResult{};
    acp.http_result.ok = true;
    acp.http_result.status = 400;
    acp.http_result.body = R"({"error": "bad request"})";
    ctrl.send_message("/balance");
    EXPECT_NE(state->items.back().text.find("bad request"), std::string::npos);
}

TEST(UiController, SessionSwitchAndDelete) {
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
    EXPECT_EQ(state->current_session, "new");
    EXPECT_EQ(acp.last_switch, "new");

    // delete_session：非当前 → 列表刷新，保留 overlay
    state->current_session = "new";
    ctrl.delete_session(a);
    EXPECT_EQ(acp.sessions.size(), 2u);

    // delete_session 当前 → 建新会话 + 清空
    ctrl.delete_session(b);
    EXPECT_GE(acp.create_calls, 1);
    EXPECT_EQ(state->current_session, "sess-new");
}

// ---------------------------------------------------------------------------
// 工具折叠：点击命中判定 + 屏幕坐标换算（对照 FTXUI v7.0.0 frame 滚动语义）
// ---------------------------------------------------------------------------
namespace {

// FTXUI frame.cpp/focus.cpp 的实际滚动量（v7.0.0 源码语义）：
//   focusPositionRelative(0, yfrac) → focused.box.y_min = int(total·yfrac)
//   dy = focus_y − external/2，clamp [0, internal − external − 1]
//   其中 external = vh−1，internal = max(total, external)
int ftxui_dy(int total, int vh, float yfrac) {
    int dy = (int)((float)total * yfrac) - (vh - 1) / 2;
    dy = std::max(0, std::min(std::max(total, vh - 1) - vh, dy));
    return dy;
}

ConvItem FoldableBash() {
    ConvItem item;
    item.kind = ItemKind::ToolCall;
    item.tool_name = "bash";
    item.has_result = true;
    item.tool_success = true;
    item.result_text = "hello\nworld";
    return item;
}

}  // namespace

TEST(ToolRender, ScrollFormulaMatchesFtxui) {
    // 点击/悬停换算必须与 FTXUI frame 的实际滚动位置一致，
    // 否则命中行整体错位（点 ▾/more... 无反应）。逐组合对照。
    std::vector<int> totals = {10, 30, 100};
    for (int vh : {14, 15, 24, 25}) {
        // 内容行数贴近视口高度的边界（此前 total==vh+1 时 dy 多减 1 行）
        for (int extra : {-2, -1, 0, 1, 2}) totals.push_back(vh + extra);
        for (int total : totals)
            for (int sp : {0, total / 3, total / 2, (9 * total) / 10, total})
                for (bool auto_scroll : {false, true}) {
                    float yfrac = auto_scroll
                                      ? 1.f
                                      : std::min(1.0f, (float)sp / std::max(1, total));
                    int expected = ftxui_dy(total, vh, yfrac);
                    // 视口首行（my == conv_top）的内容行号即 dy
                    int row = content_row_at_math(0, 0, vh, total, sp, auto_scroll);
                    EXPECT_EQ(row, expected)
                        << "total=" << total << " vh=" << vh << " sp=" << sp
                        << " auto=" << auto_scroll << " yfrac=" << yfrac;
                }
    }
}

TEST(ToolRender, ScreenToContentRowBounds) {
    // 视口外 → -1
    EXPECT_EQ(content_row_at_math(-1, 2, 16, 100, 0, true), -1);
    EXPECT_EQ(content_row_at_math(16, 2, 16, 100, 0, true), -1);
    // 顶部（未滚动）
    EXPECT_EQ(content_row_at_math(2, 2, 16, 100, 0, false), 0);
    // 底部（自动滚动）：dy = max(0, max(total,VH−1)−VH)，最后可见行 = dy+VH−1
    EXPECT_EQ(content_row_at_math(2, 2, 16, 100, 0, true), 100 - 14);
    EXPECT_EQ(content_row_at_math(15, 2, 16, 100, 0, true), 99);  // 视口最后一行
    // 内容不足一屏 → 恒第 0 行
    EXPECT_EQ(content_row_at_math(2, 2, 16, 10, 100, false), 0);
    // 空内容 → -1
    EXPECT_EQ(content_row_at_math(2, 2, 16, 0, 0, false), -1);
}

TEST(ToolRender, FoldToggleHitRows) {
    ConvItem item = FoldableBash();

    // 展开态：仅 ▾ 命令行行可点击收回；块行/空行/普通行不可
    item.folded = false;
    EXPECT_TRUE(tool_row_is_fold_target(item, "▾ $ ls -la"));
    EXPECT_FALSE(tool_row_is_fold_target(item, "$ ls -la"));
    EXPECT_FALSE(tool_row_is_fold_target(item, "│ hello"));
    EXPECT_FALSE(tool_row_is_fold_target(item, ""));
    toggle_tool_fold(item);
    EXPECT_TRUE(item.folded);

    // 折叠态：仅 "  more..." 行可点击展开
    EXPECT_TRUE(tool_row_is_fold_target(item, "  more..."));
    EXPECT_FALSE(tool_row_is_fold_target(item, " more..."));
    EXPECT_FALSE(tool_row_is_fold_target(item, "│ hello"));
    toggle_tool_fold(item);
    EXPECT_FALSE(item.folded);
}

TEST(ToolRender, FoldToggleNotOnNonFoldable) {
    // read/glob/grep 恒内联，不可折叠
    for (const char* name : {"read", "glob", "grep"}) {
        ConvItem item;
        item.kind = ItemKind::ToolCall;
        item.tool_name = name;
        item.has_result = true;
        item.tool_success = true;
        item.result_text = "x";
        EXPECT_FALSE(tool_row_is_fold_target(item, "▾ ")) << name;
        EXPECT_FALSE(tool_row_is_fold_target(item, "  more...")) << name;
    }
    // 失败结果恒展开
    ConvItem fail = FoldableBash();
    fail.tool_success = false;
    fail.error_text = "boom";
    EXPECT_FALSE(tool_row_is_fold_target(fail, "▾ "));
    EXPECT_FALSE(tool_row_is_fold_target(fail, "  more..."));
    // pending（无结果）不可折叠
    ConvItem pend = FoldableBash();
    pend.has_result = false;
    EXPECT_FALSE(tool_row_is_fold_target(pend, "~ Writing command..."));
    // 非 ToolCall 条目不可折叠
    ConvItem asst;
    asst.kind = ItemKind::Assistant;
    asst.text = "▾ whatever";
    EXPECT_FALSE(tool_row_is_fold_target(asst, "▾ whatever"));
}