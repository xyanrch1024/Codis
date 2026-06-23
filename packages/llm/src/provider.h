#pragma once

#include "types.h"

#include <string>
#include <string_view>
#include <functional>
#include <memory>

namespace opencode {

class LLMProvider {
public:
    using TokenCallback = std::function<void(std::string_view delta)>;

    virtual ~LLMProvider() = default;
    virtual std::string name() const = 0;
    virtual ChatResponse chat(const ChatRequest& req) = 0;
    virtual ChatResponse stream_chat(const ChatRequest& req, TokenCallback on_token) = 0;
};

} // namespace opencode
