add_executable(codis
    src/main.cpp
    src/tui.cpp
    src/acp_client.cpp
    src/tui.h
)

find_package(ftxui CONFIG REQUIRED)

target_link_libraries(codis PRIVATE
    CLI11::CLI11
    httplib::httplib
    nlohmann_json::nlohmann_json
    ftxui::component
    ftxui::dom
    ftxui::screen
    codis_llm
    codis_util
    Threads::Threads
)
