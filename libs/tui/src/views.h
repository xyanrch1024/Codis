#pragma once

#include "model.h"
#include "acp_client.h"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/event.hpp>

#include <functional>
#include <string>
#include <vector>
#include <utility>

namespace codis {

using ftxui::Element;
using ftxui::Event;

// 渲染上下文：视图只读此处数据，不触碰 AcpClient / TuiClient
struct ViewCtx {
    int term_w = 0;
    int term_h = 0;
    const TuiState* state = nullptr;
    std::string model;
    std::string provider;
    std::string cwd;
    std::string notice;
    std::string session_id;
    bool connected = false;
    bool yolo = false;
    bool processing = false;
    int spinner_frame = 0;
};

// ---- 对话区布局：内容元素 + 行签名（点击命中测试反查） ----
struct ConversationLayout {
    Element content;                     // 内容（已应用宽度限制，未加 frame/滚动）
    std::vector<std::string> row_sigs;   // 与内容行一一对应
    std::vector<int> row_owners;         // 每行所属 item 索引
};
ConversationLayout render_conversation(const TuiState& st, int tw, int hover_row);

// 底部状态栏（含 spinner 帧推进由调用方负责）
Element render_status_bar(const ViewCtx& ctx);

// "/" 命令补全弹窗（filtered = 与当前输入前缀匹配的命令）
Element render_cmd_palette(const std::vector<std::pair<std::string, std::string>>& filtered,
                           int selected);

// =============================================================================
// Overlay：每种面板自包含「状态 + 渲染 + 按键」。handle_key 在面板打开时恒
// 吞掉所有按键（与历史行为一致）；效果类按键（Enter/d 等）通过注入回调触发，
// 由组合根（TuiClient）接到业务层（ChatController）。
// =============================================================================

struct HelpOverlay {
    bool visible = false;

    bool handle_key(Event e);
    Element render(Element body,
                   const std::vector<std::pair<std::string, std::string>>& commands) const;
};

struct InfoOverlay {
    bool visible = false;
    int pane = 0;                       // 0=skills, 1=mcp
    int sel[2] = {0, 0};                // 每栏选中索引
    std::vector<SkillBrief> skills;
    std::vector<McpServerBrief> mcps;

    bool handle_key(Event e);
    Element render(Element body) const;
};

struct SessionsOverlay {
    bool visible = false;
    int selected = 0;
    std::vector<SessionInfo> list;

    // 效果回调（组合根注入）：Enter 激活会话 / d 删除会话
    std::function<void(const SessionInfo&)> on_activate;
    std::function<void(const SessionInfo&)> on_delete;

    bool handle_key(Event e);
    Element render(Element body, const std::string& current_session) const;
};

struct ConfirmOverlay {
    bool focus = false;                  // 按钮焦点：false=拒绝（安全默认），true=批准
    int height = 0;                      // 最近一次渲染的对话框总高（鼠标命中几何）
    std::function<void(bool)> on_respond;

    // 活动（pending_confirm 非空）时由调用方先判断，此处恒返回 true（吞掉一切）
    bool handle_key(Event e);
    Element render(Element body, const acp::ToolCallEvent& call, int remain_secs,
                   int term_w);
};

} // namespace codis
