add_library(opencode_llm STATIC
    src/client.cpp
    src/openai_compatible_provider.cpp
    src/acp_client.cpp
    src/tools/tools.cpp
)

target_include_directories(opencode_llm PUBLIC src)
target_link_libraries(opencode_llm PUBLIC
    asio::asio
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    opencode_util
)
