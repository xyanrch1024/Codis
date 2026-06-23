add_executable(opencode
    src/main.cpp
)

target_link_libraries(opencode PRIVATE
    CLI11::CLI11
    httplib::httplib
    nlohmann_json::nlohmann_json
    opencode_llm
    opencode_util
    Threads::Threads
)
