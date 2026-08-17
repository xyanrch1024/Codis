cmake_minimum_required(VERSION 3.20)
project(codis-cpp VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 测试支持（ctest；各子目录按需 add_test）
include(CTest)

# ==============================================================================
# 包管理 — 统一 vcpkg manifest 模式
# 所有第三方库由 vcpkg 管理，零 FetchContent
# ==============================================================================

find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(libs/util)
add_subdirectory(libs/protocol)
add_subdirectory(libs/tool)
add_subdirectory(libs/plugin)
add_subdirectory(libs/llm)
add_subdirectory(libs/mcp)
add_subdirectory(libs/tui)
add_subdirectory(libs/server)

# ==============================================================================
# 输出布局 — codis / codis-server 统一输出到 ${CMAKE_BINARY_DIR}/bin
# ==============================================================================
set_target_properties(codis        PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_target_properties(codis-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ==============================================================================
# 安装规则
#   bin/                codis, codis-server
#   etc/codis/          config.toml
#   lib/codis/plugins/  echo_plugin.so
# ==============================================================================
install(TARGETS codis codis-server RUNTIME DESTINATION bin)
install(FILES config/config.toml DESTINATION etc/codis)
install(DIRECTORY plugins/ DESTINATION lib/codis/plugins FILES_MATCHING PATTERN "*.so")

# ==============================================================================
# Uninstall 目标 — cmake --build build --target uninstall
# ==============================================================================
if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)
    add_custom_target(uninstall
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        COMMENT "Uninstalling codis-cpp")
endif()
