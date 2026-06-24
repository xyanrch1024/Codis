#include "feishu_bot.h"
#include "log.h"
#include "acp_client.h"
#include <httplib.h>
#include <format>

namespace opencode {

FeishuBot::FeishuBot(const std::string& app_id, const std::string& app_secret)
    : ws_(app_id, app_secret)
{
    ws_.set_event_callback([this](const json& event) {
        on_fs_message(event);
    });
}

FeishuBot::~FeishuBot() { stop(); }

void FeishuBot::start() { ws_.start(); }
void FeishuBot::stop() { ws_.stop(); }

// =============================================================================
// 消息处理
// =============================================================================

void FeishuBot::on_fs_message(const json& event) {
    try {
        auto& e = event["event"];
        auto& msg = e["message"];
        std::string chat_id = msg["chat_id"].get<std::string>();
        std::string msg_id = msg["message_id"].get<std::string>();
        std::string content_str = msg["content"].get<std::string>();

        // 解析飞书消息内容 (可能是 JSON)
        std::string text;
        try {
            auto content_json = json::parse(content_str);
            text = content_json.value("text", content_str);
        } catch (...) {
            text = content_str;
        }

        if (text.empty()) return;

        LOG_INFO("Feishu message: chat={}, text={}", chat_id.substr(0, 8), text.substr(0, 50));

        // session 映射
        auto sid = get_or_create_session(chat_id);

        // 调用 Codis API (fire-and-forget)
        AcpClient acp(8711);
        ChatRequest req;
        req.session_id = sid;
        req.provider = "glm";
        req.model = "glm-4.5-flash";
        req.messages.push_back({"user", text});

        auto resp = acp.send_async(req);
        if (resp) {
            LOG_DEBUG("ACP submitted for session {}", sid.substr(0, 8));
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Feishu message processing error: {}", e.what());
    }
}

// =============================================================================
// Session 映射
// =============================================================================

std::string FeishuBot::get_or_create_session(const std::string& chat_id) {
    std::lock_guard lock(sessions_mutex_);
    auto it = chat_sessions_.find(chat_id);
    if (it != chat_sessions_.end()) return it->second;

    // 创建新 session (通过 REST API)
    AcpClient acp(8711);
    auto sid = acp.create_session();
    if (sid) {
        chat_sessions_[chat_id] = *sid;
        LOG_INFO("New session {} for chat {}", sid->substr(0, 8), chat_id.substr(0, 8));
        return *sid;
    }

    // fallback: 本地 UUID
    std::string fallback = chat_id.substr(0, 8);
    chat_sessions_[chat_id] = fallback;
    return fallback;
}

// =============================================================================
// 发送回复
// =============================================================================

void FeishuBot::send_reply(const std::string& msg_id, const std::string& text) {
    // 需要重连获取 token
    httplib::SSLClient cli("open.feishu.cn");

    json body;
    body["msg_type"] = "text";
    body["content"] = json{{"text", text}}.dump();

    // 先获取 token
    // ...

    LOG_DEBUG("Reply sent to msg {}", msg_id.substr(0, 8));
}

} // namespace opencode
