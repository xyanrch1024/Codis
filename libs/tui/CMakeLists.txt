add_executable(codis
    src/main.cpp
    src/tui.cpp
    src/controller.cpp
    src/views.cpp
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
    codis_protocol
    codis_util
    Threads::Threads
)

# 纯逻辑单测（overlay 按键 / 模型事件 / 命令分发，无终端无网络）
if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    add_executable(codis_ui_test
        tests/ui_logic_test.cpp
        src/views.cpp
        src/controller.cpp
        src/acp_client.cpp
    )
    target_link_libraries(codis_ui_test PRIVATE
        GTest::gtest_main
        httplib::httplib
        nlohmann_json::nlohmann_json
        ftxui::component
        ftxui::dom
        ftxui::screen
        codis_protocol
        codis_util
        Threads::Threads
    )
    target_include_directories(codis_ui_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    include(GoogleTest)
    gtest_discover_tests(codis_ui_test)
endif()
