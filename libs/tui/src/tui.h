#pragma once

#include "model.h"
#include "acp_client.h"
#include "controller.h"
#include "views.h"

#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>

namespace codis {

// 组合根：事件循环 + 组装。输入/渲染/业务已分别下沉到
// ChatController（业务）与 views/（渲染），本类只做键盘映射与编排。
class TuiClient {
public:
    TuiClient(int server_port, std::string model, std::string provider,
              std::string session_arg, bool auto_approve = false);
    int run();

private:
    void show_notice(const std::string& msg);
    void respond_confirm(bool approve);

    int server_port_;
    std::string session_arg_;
    bool auto_approve_ = false;
    AcpClient acp_;
    std::shared_ptr<TuiState> state_;
    ChatController controller_;

    // Overlay（视图层状态自包含）
    HelpOverlay help_;
    InfoOverlay info_;
    SessionsOverlay sessions_;
    ModelOverlay model_picker_;
    ConfirmOverlay confirm_;

    std::function<void()> post_job_;  // run() 内设置：合并刷新后的渲染请求
    std::function<void()> exit_loop_; // run() 内设置为 screen.ExitLoopClosure()

    // Conversation scrolling（行级偏移，1 像素 = 1 终端行）
    int scroll_px_ = 0;         // 距顶部偏移行数；0 = 顶部
    int max_scroll_ = 0;        // 每帧由 renderer 更新：最大可滚行数
    bool auto_scroll_ = true;

    // 最近一帧的对话布局（点击命中测试反查内容行）
    ConversationLayout conv_layout_;

    // Spinner 动画帧索引（每渲染推进一次）
    int spinner_frame_ = 0;

    // 状态栏瞬时提示（复制/取消等）；定时线程轮询 notice_pending_ 触发自动消失
    std::string notice_;
    std::chrono::steady_clock::time_point notice_at_;
    std::atomic<bool> notice_pending_{false};

    std::string cwd_;
};

} // namespace codis