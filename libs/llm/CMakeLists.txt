add_library(codis_llm STATIC
    src/llm_http_client.cpp
    src/openai_compatible_provider.cpp
)

target_include_directories(codis_llm PUBLIC src ${SQLite3_INCLUDE_DIR})
target_link_libraries(codis_llm PUBLIC
    httplib::httplib
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    codis_protocol
    codis_util
    SQLite::SQLite3
)
