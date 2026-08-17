// SessionHub — 会话运行时状态中枢。
// 职责：每 session 的连接表（FrameQueue）、任务状态机（Idle/Running/Compacting）、
// pending 队列、取消标志、Ask 权限确认挂起。所有跨线程状态由单一互斥保护。
// 原 CodisServer 内联的 sessions_/sessions_mutex_/SessionState 迁入此处（见 4be62d6 之前）。

#pragma once

#include "messages.h"
#include "tool.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>

namespace codis {

// =============================================================================
// FrameQueue — 每连接帧缓冲队列（ACP 循环 push，WS 发送线程 pop）
// =============================================================================

class FrameQueue {
public:
    void push(std::string frame);
    std::string pop();   // 阻塞；队列关闭（close）后返回空串
    // 非阻塞轮询：超时内拿到帧返回 true 并写入 out；关闭/超时返回 false
    bool try_pop(std::string& out, std::chrono::milliseconds timeout);
    void close();

private:
    std::queue<std::string> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> closed_{false};
};

// =============================================================================
// Ask 权限工具的挂起确认：服务端等待任意连接的 confirm_ack 回执
// =============================================================================

struct PendingConfirm {
    ToolCall call;
    bool approved = false;
    bool answered = false;
    std::mutex mutex;
    std::condition_variable cv;
};

// 会话任务阶段：Idle/Running/Compacting 互斥（原 SessionState.processing 布尔，
// 状态机化后语义不变：一次只能有一个任务；Compacting 期间 agent 请求入队补跑）
enum class SessionPhase { Idle, Running, Compacting };

struct SessionState {
    std::unordered_map<std::string, std::shared_ptr<FrameQueue>> conns;
    SessionPhase phase = SessionPhase::Idle;
    // 客户端取消当前任务 — 用 shared_ptr 持有，保证 run 线程在 session 条目被
    // 清除后仍能安全读写该标志
    std::shared_ptr<std::atomic<bool>> cancel_requested{std::make_shared<std::atomic<bool>>(false)};
    std::deque<ChatRequest> pending;  // 处理期间到达的请求，当前轮结束后按序补跑
    // confirm_id → 挂起的工具确认（等待 ack 帧唤醒）
    std::unordered_map<std::string, std::shared_ptr<PendingConfirm>> pending_confirms;
};

// =============================================================================
// SessionHub
// =============================================================================

class SessionHub {
public:
    SessionHub() = default;
    SessionHub(const SessionHub&) = delete;
    SessionHub& operator=(const SessionHub&) = delete;

    // ---- 连接管理（惰性创建 session 条目） ----

    void attach_connection(const std::string& sid, const std::string& conn_id,
                           std::shared_ptr<FrameQueue> queue);
    // 断线重连身份迁移：把会话上仍在执行任务的投递目标（旧 conn_id）指向新队列
    void migrate_reconnect(const std::string& old_conn, const std::string& sid,
                           std::shared_ptr<FrameQueue> queue);
    // 按队列值清理：正常键 + 重连别名键一并移除，连接清空则移除会话条目
    void detach_connection(const std::string& sid, std::shared_ptr<FrameQueue> queue);
    // 把 conn_id 从当前 session 移到目标 session，返回被迁移的连接队列
    // （nullptr = conn 不存在）；是否推送 connected_frame 由调用方决定
    std::shared_ptr<FrameQueue> move_connection(const std::string& conn_id, const std::string& new_sid);

    // ---- 广播 ----

    // conn_id 为空 → 广播该会话全部连接；否则定向投递（缺连接时打日志不抛）
    void broadcast(const std::string& session_id, const std::string& conn_id,
                   const std::string& frame);

    // ---- 任务状态机 ----

    // 尝试启动 agent 任务：Idle→Running 返回 true（调用方接管）；否则请求入队返回 false
    bool start_task(const std::string& session_id, const ChatRequest& req);
    // 每次任务结束后调用：pending 有 → 取下一个（保持 Running）；空 → 回 Idle 返回 nullopt
    std::optional<ChatRequest> next_task_or_idle(const std::string& session_id);
    // compact 抢占：Idle→Compacting 返回 true；任何任务进行中返回 false
    bool start_compact(const std::string& session_id);
    // compact 结束（含失败路径，恢复 Idle）：有排队请求取第一个供调用方补跑
    std::optional<ChatRequest> finish_compact(const std::string& session_id);

    // ---- 取消 ----

    // 会话取消标志（不存在则惰性创建）
    std::shared_ptr<std::atomic<bool>> cancel_token(const std::string& session_id);
    // 置取消标志 + 清空排队请求 + 拒绝所有挂起确认并唤醒
    void request_cancel(const std::string& session_id);

    // ---- Ask 权限确认 ----

    // 登记挂起确认，返回 slot（confirm_id 由调用方生成）
    std::shared_ptr<PendingConfirm> add_confirm(const std::string& session_id,
                                                const std::string& confirm_id);
    // 按 confirm_id 全局查找（任意会话）
    std::shared_ptr<PendingConfirm> find_confirm(const std::string& confirm_id);
    void erase_confirm(const std::string& session_id, const std::string& confirm_id);

private:
    std::unordered_map<std::string, SessionState> sessions_;
    std::mutex sessions_mutex_;
};

} // namespace codis
