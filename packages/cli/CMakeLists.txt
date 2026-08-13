add_executable(opencode
    src/main.cpp
    src/tui.cpp
    src/tui.h
)

find_package(ftxui CONFIG REQUIRED)

target_link_libraries(opencode PRIVATE
    CLI11::CLI11
    httplib::httplib
    nlohmann_json::nlohmann_json
    ftxui::component
    ftxui::dom
    ftxui::screen
    opencode_llm
    opencode_util
    Threads::Threads
)
