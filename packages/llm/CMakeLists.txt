add_library(opencode_llm STATIC
    src/client.cpp
    src/openai_compatible_provider.cpp
    src/acp_client.cpp
    src/tools/tools.cpp
    src/session_store.cpp
    src/context_source.cpp
    src/plugin_loader.cpp
)

target_include_directories(opencode_llm PUBLIC src ${SQLite3_INCLUDE_DIR})
target_link_libraries(opencode_llm PUBLIC
    asio::asio
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    opencode_util
    SQLite::SQLite3
    ${CMAKE_DL_LIBS}
)
