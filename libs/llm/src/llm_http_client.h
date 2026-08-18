#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <map>
#include <atomic>

#include <nlohmann/json.hpp>
#include <httplib.h>
#include "messages.h"

namespace codis {

using json = nlohmann::json;

class LLMHttpClient {
public:
    using TokenCallback    = std::function<void(std::string_view delta)>;
    using ReasoningCallback = std::function<void(std::string_view delta)>;
    using DoneCallback     = std::function<void(std::string content, bool success,
                                                 LlmErrorCode code, std::string error)>;

    LLMHttpClient();

    void stream_post(const std::string& url,
                     const std::string& api_key,
                     const json& body,
                     TokenCallback on_token,
DoneCallback on_done,
                                int timeout_seconds,
                                bool non_stream,
                                std::string* reasoning_out = nullptr,
                                ReasoningCallback on_reasoning = nullptr,
                                std::atomic<bool>* abort_flag = nullptr,
                                const std::string& proxy = "");

private:
    // 解析一条 SSE 行；遇到 [DONE] 返回 false。delta 实时回调。
    bool parse_sse_line(const std::string& line,
                        std::map<int, json>& tool_calls,
                        std::string& reasoning,
                        TokenCallback& on_token,
                        ReasoningCallback& on_reasoning);
};

} // namespace codis
