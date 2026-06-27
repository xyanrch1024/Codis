add_executable(opencode
    src/main.cpp
    src/tui.cpp
    src/tui.h
)

set(FTXUI_SRC /home/wsl-ubuntu-1/vcpkg/buildtrees/ftxui/src/v7.0.0-9aedf7bb2b.clean)
set(FTXUI_BUILD /tmp/ftxui_build)
target_include_directories(opencode PRIVATE ${FTXUI_SRC}/include)
target_link_libraries(opencode PRIVATE
    CLI11::CLI11
    httplib::httplib
    nlohmann_json::nlohmann_json
    -Wl,--start-group
    ${FTXUI_BUILD}/libftxui-component.a
    ${FTXUI_BUILD}/libftxui-dom.a
    ${FTXUI_BUILD}/libftxui-screen.a
    -Wl,--end-group
    opencode_llm
    opencode_util
    Threads::Threads
)
