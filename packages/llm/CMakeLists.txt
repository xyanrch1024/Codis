add_library(codis_llm STATIC
    src/client.cpp
    src/openai_compatible_provider.cpp
    src/acp_client.cpp
    src/tools/tools.cpp
    src/session_store.cpp
    src/context_source.cpp
)

target_include_directories(codis_llm PUBLIC src ${SQLite3_INCLUDE_DIR})
target_link_libraries(codis_llm PUBLIC
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    codis_util
    SQLite::SQLite3
)
