#pragma once

#include "provider.h"
#include "llm_http_client.h"
#include "config.h"
#include "log.h"

#include <memory>
#include <string>

namespace codis {

class OpenAICompatibleProvider : public LLMProvider {
public:
    OpenAICompatibleProvider(const ProviderConfig& cfg)
        : name_(cfg.name)
        , api_key_(cfg.api_key)
        , model_(cfg.model)
        , base_url_(cfg.base_url)
    {
        LOG_INFO("provider '{}' registered: model={}, url={}", name_, model_, base_url_);
    }

    std::string name() const override { return name_; }

    ChatResponse chat(const ChatRequest& req) override {
        ChatResponse result;
        LOG_DEBUG("{}::chat model={} messages={}", name_, model_, req.messages.size());

        json body = build_body(req);
        body["stream"] = false;

        std::shared_ptr<LLMHttpClient> client = std::make_shared<LLMHttpClient>();

        client->stream_post(
            base_url_ + "/chat/completions",
            api_key_,
            body,
            {},
            [&](std::string content, bool success, std::string error) {
                result.content = content;
                result.success = success;
                result.error = std::move(error);
            },
            60,
            true,
            &result.reasoning_content
        );

        LOG_DEBUG("{}::chat result success={} content_len={}", name_, result.success, result.content.size());
        return result;
    }

    ChatResponse stream_chat(const ChatRequest& req, TokenCallback on_token,
                             ReasoningCallback on_reasoning = nullptr,
                             std::atomic<bool>* abort_flag = nullptr) override {
        ChatResponse result;
        LOG_DEBUG("{}::stream_chat model={}", name_, req.model);

        json body = build_body(req);
        body["stream"] = true;

        std::shared_ptr<LLMHttpClient> client = std::make_shared<LLMHttpClient>();

        client->stream_post(
            base_url_ + "/chat/completions",
            api_key_,
            body,
            std::move(on_token),
            [&](std::string, bool success, std::string error) {
                result.success = success;
                if (error == "__canceled__") {
                    result.canceled = true;
                    result.error.clear();
                } else {
                    result.error = std::move(error);
                }
            },
            120,
            false,
            &result.reasoning_content,
            std::move(on_reasoning),
            abort_flag
        );

        return result;
    }

    std::string get_model() const { return model_; }

private:
    json build_body(const ChatRequest& req) {
        json body;
        body["model"] = model_;
        body["messages"] = json::array();
        for (auto& m : req.messages) {
            json msg{{"role", m.role}, {"content", m.content}};
            if (m.tool_call_id) {
                msg["tool_call_id"] = *m.tool_call_id;
                if (m.tool_name) {
                    std::string args_str = "{}";
                    if (m.tool_arguments) args_str = m.tool_arguments->dump();
                    msg["tool_calls"] = json::array({{
                        {"id", *m.tool_call_id},
                        {"type", "function"},
                        {"function", {{"name", *m.tool_name}, {"arguments", args_str}}}
                    }});
                }
            }
            body["messages"].push_back(msg);
        }
        if (req.max_tokens)   body["max_tokens"]  = *req.max_tokens;
        if (!req.tools.empty()) body["tools"] = req.tools;
        return body;
    }

    std::string name_;
    std::string api_key_;
    std::string model_;
    std::string base_url_;
};

} // namespace codis
