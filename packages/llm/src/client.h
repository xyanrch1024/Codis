#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <memory>

#include <nlohmann/json.hpp>
#include <httplib.h>

namespace opencode {

using json = nlohmann::json;

class LLMHttpClient {
public:
    using TokenCallback = std::function<void(std::string_view delta)>;
    using DoneCallback  = std::function<void(std::string content, bool success, std::string error)>;

    LLMHttpClient();

    void stream_post(const std::string& url,
                     const std::string& api_key,
                     const json& body,
                     TokenCallback on_token,
                     DoneCallback on_done,
                     int timeout_seconds = 60,
                     bool non_stream = false);

private:
    void parse_sse_line(const std::string& line, TokenCallback& on_token);
};

} // namespace opencode
