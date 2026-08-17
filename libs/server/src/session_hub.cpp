#include "session_hub.h"
#include "log.h"
#include "str_util.h"

#include <chrono>
#include <string>

namespace codis {

// =============================================================================
// FrameQueue
// =============================================================================

void FrameQueue::push(std::string frame) {
    { std::lock_guard lock(mutex_); queue_.push(std::move(frame)); }
    cv_.notify_one();
}

std::string FrameQueue::pop() {
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return !queue_.empty() || closed_; });
    if (queue_.empty()) return "";
    auto frame = std::move(queue_.front()); queue_.pop();
    return frame;
}

bool FrameQueue::try_pop(std::string& out, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [this] { return !queue_.empty() || closed_; }))
        return false;
    if (queue_.empty()) return false;
    out = std::move(queue_.front()); queue_.pop();
    return true;
}

void FrameQueue::close() { closed_ = true; cv_.notify_all(); }

// =============================================================================
// SessionHub
// =============================================================================

void SessionHub::attach_connection(const std::string& sid, const std::string& conn_id,
                                   std::shared_ptr<FrameQueue> queue) {
    std::lock_guard lock(sessions_mutex_);
    sessions_[sid].conns[conn_id] = std::move(queue);
}

void SessionHub::migrate_reconnect(const std::string& old_conn, const std::string& sid,
                                   std::shared_ptr<FrameQueue> queue) {
    std::lock_guard lock(sessions_mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        auto oit = it->second.conns.find(old_conn);
        if (oit != it->second.conns.end()) {
            it->second.conns.erase(oit);
            break;
        }
    }
    sessions_[sid].conns[old_conn] = std::move(queue);
}

void SessionHub::detach_connection(const std::string& sid, std::shared_ptr<FrameQueue> queue) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(sid);
    if (it == sessions_.end()) return;
    auto& conns = it->second.conns;
    for (auto cit = conns.begin(); cit != conns.end();) {
        if (cit->second == queue) cit = conns.erase(cit);
        else ++cit;
    }
    if (conns.empty()) sessions_.erase(it);
}

std::shared_ptr<FrameQueue> SessionHub::move_connection(const std::string& conn_id,
                                                        const std::string& new_sid) {
    std::shared_ptr<FrameQueue> queue;
    {
        std::lock_guard lock(sessions_mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            auto qit = it->second.conns.find(conn_id);
            if (qit != it->second.conns.end()) {
                queue = qit->second;
                it->second.conns.erase(qit);
                if (it->second.conns.empty())
                    sessions_.erase(it);
                break;
            }
        }
        if (queue)
            sessions_[new_sid].conns[conn_id] = queue;
    }
    return queue;
}

void SessionHub::broadcast(const std::string& session_id, const std::string& conn_id,
                           const std::string& frame) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;
    if (conn_id.empty()) {
        // conn_id 为空时发送到所有连接
        for (auto& [_, q] : it->second.conns)
            q->push(frame);
    } else {
        auto qit = it->second.conns.find(conn_id);
        if (qit != it->second.conns.end()) {
            qit->second->push(frame);
        } else {
            // 目标 conn 不存在（未带 reconnect 重连/换端口/请求被其它实例接收）：
            // 打日志以便诊断"服务端有消息但客户端收不到"。
            // 正常重连场景由 migrate_reconnect 的别名迁移接管（见 WS reconnect）
            LOG_WARN("broadcast drop: conn {} not in session {} ({} conns attached)",
                     conn_id, session_id.substr(0, 8), it->second.conns.size());
        }
    }
}

bool SessionHub::start_task(const std::string& session_id, const ChatRequest& req) {
    std::lock_guard lock(sessions_mutex_);
    auto& st = sessions_[session_id];
    if (st.phase == SessionPhase::Idle) {
        st.phase = SessionPhase::Running;
        return true;
    }
    st.pending.push_back(req);
    return false;
}

std::optional<ChatRequest> SessionHub::next_task_or_idle(const std::string& session_id) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return std::nullopt;
    if (!it->second.pending.empty()) {
        auto next = std::move(it->second.pending.front());
        it->second.pending.pop_front();
        return next;
    }
    it->second.phase = SessionPhase::Idle;
    return std::nullopt;
}

bool SessionHub::start_compact(const std::string& session_id) {
    std::lock_guard lock(sessions_mutex_);
    auto& st = sessions_[session_id];
    if (st.phase != SessionPhase::Idle) return false;
    st.phase = SessionPhase::Compacting;
    return true;
}

std::optional<ChatRequest> SessionHub::finish_compact(const std::string& session_id) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return std::nullopt;
    it->second.phase = SessionPhase::Idle;
    if (it->second.pending.empty()) return std::nullopt;
    auto next = std::move(it->second.pending.front());
    it->second.pending.pop_front();
    return next;
}

std::shared_ptr<std::atomic<bool>> SessionHub::cancel_token(const std::string& session_id) {
    std::lock_guard lock(sessions_mutex_);
    return sessions_[session_id].cancel_requested;
}

void SessionHub::request_cancel(const std::string& session_id) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) return;
    it->second.cancel_requested->store(true);
    it->second.pending.clear();  // 取消后排队消息一并清空
    // 挂起的权限确认立即终止（视为拒绝），唤醒等待线程
    for (auto& [_, slot] : it->second.pending_confirms) {
        std::lock_guard lk(slot->mutex);
        slot->approved = false;
        slot->answered = true;
    }
    for (auto& [_, slot] : it->second.pending_confirms)
        slot->cv.notify_all();
}

std::shared_ptr<PendingConfirm> SessionHub::add_confirm(const std::string& session_id,
                                                        const std::string& confirm_id) {
    std::lock_guard lock(sessions_mutex_);
    auto slot = std::make_shared<PendingConfirm>();
    sessions_[session_id].pending_confirms[confirm_id] = slot;
    return slot;
}

std::shared_ptr<PendingConfirm> SessionHub::find_confirm(const std::string& confirm_id) {
    std::lock_guard lock(sessions_mutex_);
    for (auto& [_, st] : sessions_) {
        auto it = st.pending_confirms.find(confirm_id);
        if (it != st.pending_confirms.end()) return it->second;
    }
    return nullptr;
}

void SessionHub::erase_confirm(const std::string& session_id, const std::string& confirm_id) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) it->second.pending_confirms.erase(confirm_id);
}

} // namespace codis
