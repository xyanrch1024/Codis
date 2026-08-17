add_executable(codis-server
    src/main.cpp
    src/server.cpp
    src/session_store.cpp
    src/context_source.cpp
)

target_include_directories(codis-server PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/llm/src
    ${CMAKE_SOURCE_DIR}/libs/util/src
    ${CMAKE_SOURCE_DIR}/libs/plugin/include
)

target_link_libraries(codis-server PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    codis_llm
    codis_protocol
    codis_util
    codis_tool
    codis_plugin
    codis_mcp
)

# 纯逻辑单测（session_store / 上下文压缩 / tool_calls 解析 / 协议，无网络无终端）
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(codis_server_test
        tests/session_store_test.cpp
        tests/context_utils_test.cpp
        tests/tool_call_parse_test.cpp
        tests/protocol_test.cpp
        src/session_store.cpp
    )
    target_include_directories(codis_server_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/libs/tool/src
        ${CMAKE_SOURCE_DIR}/libs/util/src
    )
    target_link_libraries(codis_server_test PRIVATE
        GTest::gtest_main
        nlohmann_json::nlohmann_json
        SQLite::SQLite3
        codis_protocol
        codis_util
        Threads::Threads
    )
    include(GoogleTest)
    gtest_discover_tests(codis_server_test)
endif()
