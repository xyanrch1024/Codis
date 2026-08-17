#include "ws_gateway.h"
#include "acp.h"
#include "context_utils.h"
#include "log.h"
#include "str_util.h"

#include <thread>

namespace codis {

WsGateway::WsGateway(SessionHub& hub, SessionStore& store, AgentLoop& loop,
                     ProviderRegistry& providers, const AppConfig& config)
    : hub_(hub), store_(store), loop_(loop), providers_(providers), config_(config) {}

void WsGateway::handle_ws(const httplib::Request& req, httplib::ws::WebSocket& ws) {
    std::string sid = req.matches[1];
    if (!store_.load_session(sid))
        store_.create_session_with_id(sid);

    auto queue = std::make_shared<FrameQueue>();
    std::string conn_id = util::gen_short_id();

    hub_.attach_connection(sid, conn_id, queue);

    // 断线重连身份迁移：客户端重连时带 ?reconnect=<旧 conn_id>，
    // 把该会话上仍在执行的任务的投递目标（旧 conn_id）指向本新连接，
    // 任务输出继续送达，而不是丢失或广播给同会话所有连接。
    if (auto old_conn = req.get_param_value("reconnect");
        !old_conn.empty() && old_conn != conn_id) {
        hub_.migrate_reconnect(old_conn, sid, queue);
        LOG_INFO("WS reconnect {} -> {} session {}",
                 old_conn.substr(0, 8), conn_id.substr(0, 8), sid.substr(0, 8));
    }

    // 首帧：告知客户端其 conn_id
    queue->push(acp::connected_frame(conn_id));

    // 连接建立即推送一次 context 统计：客户端一进会话就有读数，
    // 此后每次向 LLM 发请求前再刷新（见 AgentLoop::run_task_inner）
    {
        auto hist = store_.load_messages(sid);
        auto prov = providers_.get(providers_.default_name());
        int64_t max_ctx = kDefaultMaxContext;
        if (prov) max_ctx = provider_max_context(config_, providers_, (*prov)->name());
        queue->push(acp::context_stats_frame(context_utils::est_tokens(hist), max_ctx));
    }

    LOG_INFO("WS connection attached to session {} conn_id={}",
             sid.substr(0, 8), conn_id);

    // 发送线程：从 queue 取帧 → ws.send；队列关闭（pop 返回空）→ 关闭连接
    std::thread sender([queue, &ws]() {
        while (true) {
            auto frame = queue->pop();
            if (frame.empty()) { ws.close(); break; }
            ws.send(frame);
        }
    });

    // 读循环（全双工）：接收客户端 request 帧 + 检测断开（read 返回 Fail）
    std::string msg;
    while (ws.read(msg) != httplib::ws::ReadResult::Fail) {
        auto event = acp::parse_frame(msg);
        if (!event) {
            LOG_WARN("WS request frame parse failed: {}", msg);
            ws.send(acp::error_frame("invalid frame"));
            continue;
        }
        if (event->type != acp::EventType::request &&
            event->type != acp::EventType::switch_session &&
            event->type != acp::EventType::cancel &&
            event->type != acp::EventType::confirm_ack &&
            event->type != acp::EventType::compact) {
            LOG_WARN("WS unexpected frame type: {}", acp::to_string(event->type));
            ws.send(acp::error_frame("unsupported frame type"));
            continue;
        }
        try {
            dispatch_frame(*event, sid, conn_id, ws);
        } catch (const std::exception& e) {
            LOG_ERROR("WS request processing failed: {}", e.what());
            ws.send(acp::error_frame(e.what()));
        }
    }

    queue->close();
    if (sender.joinable()) sender.join();
    hub_.detach_connection(sid, queue);
}

void WsGateway::dispatch_frame(const acp::ParsedEvent& event, const std::string& sid,
                               const std::string& conn_id, httplib::ws::WebSocket& ws) {
    if (event.type == acp::EventType::confirm_ack) {
        // 工具确认回执：唤醒等待该 confirm_id 的挂起确认
        std::string confirm_id = event.data.value("confirm_id", "");
        bool approved = event.data.value("approved", false);
        auto slot = hub_.find_confirm(confirm_id);
        if (slot) {
            {
                std::lock_guard lk(slot->mutex);
                slot->approved = approved;
                slot->answered = true;
            }
            slot->cv.notify_one();
            LOG_INFO("confirm_ack {} approved={}", confirm_id, approved);
        } else {
            LOG_WARN("confirm_ack for unknown confirm_id: {}", confirm_id);
        }
        return;
    }
    if (event.type == acp::EventType::switch_session) {
        std::string target_sid = event.data.value("session_id", "");
        if (target_sid.empty()) {
            ws.send(acp::error_frame("switch requires session_id"));
            return;
        }
        if (!store_.load_session(target_sid))
            store_.create_session_with_id(target_sid);
        if (!hub_.move_connection(conn_id, target_sid)) {
            LOG_ERROR("WS switch failed: conn {} not found", conn_id);
            ws.send(acp::error_frame("conn not found"));
        } else {
            LOG_INFO("WS switch conn={} -> session={}",
                     conn_id.substr(0, 8), target_sid.substr(0, 8));
        }
        return;
    }
    if (event.type == acp::EventType::cancel) {
        // 取消当前 session 正在执行的任务（LLM 流 + 工具循环）
        std::string target_sid = event.data.value("session_id", "");
        if (target_sid.empty()) target_sid = sid;
        hub_.request_cancel(target_sid);
        LOG_INFO("session {} cancel requested by conn {}",
                 target_sid.substr(0, 8), conn_id.substr(0, 8));
        return;
    }
    if (event.type == acp::EventType::compact) {
        // 上下文压缩：异步执行（含一次 LLM 摘要调用），完成后广播 compacted 帧
        std::string target_sid = event.data.value("session_id", "");
        if (target_sid.empty()) target_sid = sid;
        int keep = event.data.value("keep", context_utils::kCompactDefaultKeep);
        std::thread([this, target_sid, keep]() {
            auto next = loop_.run_compact(target_sid, keep);
            // 压缩期间排队的消息：逐个补跑（复用 ACP 循环，其内部迭代清空 pending）
            if (next) {
                std::thread([this, target_sid, req = std::move(*next)]() mutable {
                    loop_.run_task(target_sid, "", std::move(req));
                }).detach();
            }
        }).detach();
        return;
    }

    auto chat_req = ChatRequest::from_json(event.data);
    // 客户端可能已通过 switch 切到其它 session，优先用帧内 session_id
    std::string target_sid = event.data.value("session_id", "");
    if (target_sid.empty()) target_sid = sid;

    // 全双工入口：追加 user 消息 → processing/pending 检查 → 启动 ACP 循环
    if (target_sid.empty() || !store_.load_session(target_sid))
        throw std::runtime_error("session not found: " + target_sid);

    bool has_msg = false;
    for (auto& m : chat_req.messages)
        if (m.role == "user" && !m.content.empty()) has_msg = true;

    if (!has_msg) return;

    for (auto it = chat_req.messages.rbegin(); it != chat_req.messages.rend(); ++it) {
        if (it->role == "user" && !it->content.empty()) {
            // 首条消息时用其内容生成会话标题
            if (store_.message_count(target_sid) == 0) {
                auto title = util::make_session_title(it->content);
                if (!title.empty()) store_.set_title(target_sid, title);
            }
            store_.append_message(target_sid, *it);
            break;
        }
    }

    // 检查是否有 LLM 正在运行（按 session）
    // 运行中则排队，当前轮结束后自动补跑，避免消息被静默丢弃
    if (hub_.start_task(target_sid, chat_req)) {
        std::thread([this, target_sid, conn_id, req = std::move(chat_req)]() mutable {
            loop_.run_task(target_sid, conn_id, std::move(req));
        }).detach();
    }
}

void WsGateway::handle_switch(const httplib::Request& req, httplib::Response& res) {
    try {
        auto body = json::parse(req.body);
        auto conn_id = body.value("conn_id", "");
        auto new_sid = body.value("session_id", "");

        if (conn_id.empty() || new_sid.empty()) {
            res.status = 400;
            res.set_content(R"({"error":"conn_id and session_id required"})", "application/json");
            return;
        }

        if (!store_.load_session(new_sid))
            store_.create_session_with_id(new_sid);

        auto queue = hub_.move_connection(conn_id, new_sid);
        if (!queue) {
            res.status = 400;
            res.set_content(R"({"error":"conn_id not found"})", "application/json");
            return;
        }
        queue->push(acp::connected_frame(conn_id));

        res.set_content(R"({"status":"ok"})", "application/json");
    } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
}

} // namespace codis
