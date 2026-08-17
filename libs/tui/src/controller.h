#pragma once

#include "model.h"
#include "acp_client.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace codis {

// 业务层对视图层的回调（由组合根 TuiClient 注入）
struct UiCallbacks {
    std::function<void()> exit;                                   // /exit
    std::function<void()> notify;                                 // 请求重绘
    std::function<void(const std::string&)> notice;               // 状态栏瞬时提示
    std::function<void()> reset_scroll;                           // 滚动复位到底部
    std::function<void()> show_help;                              // 打开 Help 面板
    std::function<void(std::vector<SessionInfo>, bool reset_selection)> show_sessions;
    std::function<void(std::vector<SkillBrief>, std::vector<McpServerBrief>)> show_info;
    std::function<void()> hide_sessions;                          // 关闭 Sessions 面板
};

// 业务逻辑层：命令分发、请求构建、会话管理、SSE 接线。
// 不触碰任何渲染/输入；通过 UiCallbacks 与视图交互。
class ChatController {
public:
    ChatController(AcpClient& acp, std::shared_ptr<TuiState> state,
                   std::string model, std::string provider,
                   bool auto_approve, int server_port);

    void set_callbacks(UiCallbacks cb) { cb_ = std::move(cb); }
    void set_model_provider(std::string model, std::string provider);

    // 命令分发（/help、/sessions、/balance... 及普通消息）
    void send_message(const std::string& text);
    void send_request(const std::string& text);
    void flush_pending();

    // 会话操作
    void switch_session(const SessionInfo& s);
    void delete_session(const SessionInfo& s);
    void open_sessions();          // Ctrl+S / /sessions

    // 取消当前任务（ESC 双击）
    void cancel_task();

    // 网络接线
    void connect_sse();
    AcpClient::Callbacks build_callbacks();
    void load_history(const std::vector<Message>& msgs);

    // 只读状态（视图层读取）
    const std::string& model() const { return model_; }
    const std::string& provider() const { return provider_; }
    bool& yolo() { return yolo_; }
    bool auto_approve() const { return auto_approve_; }
    std::shared_ptr<TuiState> state() const { return state_; }

private:
    void cmd_clear();
    void cmd_compact(const std::string& line);
    void cmd_delete_all();
    void cmd_balance(const std::string& line);
    void cmd_model(const std::string& line);

    AcpClient& acp_;
    std::shared_ptr<TuiState> state_;
    UiCallbacks cb_;
    std::string model_;
    std::string provider_;
    bool auto_approve_ = false;
    bool yolo_ = false;  // /yolo：所有 Ask 工具自动批准（deny 仍由服务端拦截）
    int server_port_ = 8711;
};

} // namespace codis