#include "feishu_bot.h"
#include "log.h"
#include "acp_client.h"
#include <httplib.h>
#include <chrono>

namespace opencode {

FeishuBot::FeishuBot(const std::string& app_id, const std::string& app_secret)
    : app_id_(app_id), app_secret_(app_secret)
{}

FeishuBot::~FeishuBot() { stop(); }

void FeishuBot::start() {
    running_ = true;
    worker_ = std::thread([this] { poll_loop(); });
}

void FeishuBot::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

// =============================================================================
// Tenant Token
// =============================================================================

std::string FeishuBot::get_tenant_token() {
    if (!token_.empty() && token_expire_ > time(nullptr))
        return token_;

    httplib::SSLClient cli("open.feishu.cn");
    json body{{"app_id", app_id_}, {"app_secret", app_secret_}};

    auto res = cli.Post("/open-apis/auth/v3/tenant_access_token/internal",
        body.dump(), "application/json");

    if (!res || res->status != 200) return "";

    auto j = json::parse(res->body);
    if (j.value("code", -1) != 0) return "";

    token_ = j["tenant_access_token"].get<std::string>();
    token_expire_ = time(nullptr) + j.value("expire", 7200) - 60;
    LOG_INFO("Tenant token refreshed");
    return token_;
}

// =============================================================================
// HTTP 轮询
// =============================================================================

void FeishuBot::poll_loop() {
    while (running_) {
        auto token = get_tenant_token();
        if (token.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // 获取最近的消息列表
        httplib::SSLClient cli("open.feishu.cn");
        std::string path = "/open-apis/im/v1/messages?"
            "receive_id_type=tenant&page_size=10&sort_type=ByCreateTimeDesc";

        httplib::Headers headers = {
            {"Authorization", "Bearer " + token}
        };

        auto res = cli.Get(path, headers);
        if (!res || res->status != 200) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        try {
            auto j = json::parse(res->body);
            if (j.value("code", -1) != 0) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }

            auto items = j["data"].value("items", json::array());
            for (auto it = items.rbegin(); it != items.rend(); ++it) {
                auto& item = *it;
                std::string msg_id = item["message_id"].get<std::string>();
                std::string msg_type = item["msg_type"].get<std::string>();

                // 去重
                {
                    std::lock_guard lock(mutex_);
                    if (last_msg_[msg_type] == msg_id) continue;
                    last_msg_[msg_type] = msg_id;
                }

                if (msg_type != "text") continue;

                std::string chat_id = item["chat_id"].get<std::string>();
                std::string content_str = item["body"].value("content", "{}");
                std::string text;
                try {
                    auto cj = json::parse(content_str);
                    text = cj.value("text", "");
                } catch (...) { text = content_str; }

                if (text.empty()) continue;

                LOG_INFO("Message: chat={} text={}", chat_id.substr(0, 8), text.substr(0, 40));

                // session 映射 + ACP
                auto sid = get_or_create_session(chat_id);
                AcpClient acp(8711);
                ChatRequest req;
                req.session_id = sid;
                req.provider = "glm";
                req.model = "glm-4.5-flash";
                req.messages.push_back({"user", text});
                acp.send_async(req);
            }

        } catch (const std::exception& e) {
            LOG_ERROR("Poll error: {}", e.what());
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// =============================================================================
// Session
// =============================================================================

std::string FeishuBot::get_or_create_session(const std::string& chat_id) {
    std::lock_guard lock(mutex_);
    auto it = chat_sessions_.find(chat_id);
    if (it != chat_sessions_.end()) return it->second;

    AcpClient acp(8711);
    auto sid = acp.create_session();
    if (sid) {
        chat_sessions_[chat_id] = *sid;
        return *sid;
    }
    return chat_id.substr(0, 8);
}

// =============================================================================
// 发送消息
// =============================================================================

void FeishuBot::send_message(const std::string& chat_id, const std::string& text) {
    auto token = get_tenant_token();
    if (token.empty()) return;

    httplib::SSLClient cli("open.feishu.cn");
    httplib::Headers headers = {{"Authorization", "Bearer " + token}};

    json body;
    body["receive_id"] = chat_id;
    body["msg_type"] = "text";
    body["content"] = json{{"text", text}}.dump();

    cli.Post("/open-apis/im/v1/messages?receive_id_type=chat_id",
        headers, body.dump(), "application/json");
}

} // namespace opencode
