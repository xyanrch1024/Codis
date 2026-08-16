#pragma once

#include "messages.h"

#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <atomic>

namespace codis {

class LLMProvider {
public:
    using TokenCallback    = std::function<void(std::string_view delta)>;
    using ReasoningCallback = std::function<void(std::string_view delta)>;

    virtual ~LLMProvider() = default;
    virtual std::string name() const = 0;
    virtual std::string get_model() const { return ""; }
    virtual ChatResponse chat(const ChatRequest& req) = 0;
    virtual ChatResponse stream_chat(const ChatRequest& req, TokenCallback on_token,
                                     ReasoningCallback on_reasoning = nullptr,
                                     std::atomic<bool>* abort_flag = nullptr) = 0;
};

} // namespace codis
