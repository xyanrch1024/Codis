add_executable(opencode-server
    src/main.cpp
    src/server.cpp
)

target_include_directories(opencode-server PRIVATE
    ${CMAKE_SOURCE_DIR}/packages/llm/src
    ${CMAKE_SOURCE_DIR}/packages/util/src
)

target_link_libraries(opencode-server PRIVATE
    asio::asio
    httplib::httplib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    opencode_llm
    opencode_util
)
