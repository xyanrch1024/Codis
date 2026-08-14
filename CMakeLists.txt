cmake_minimum_required(VERSION 3.20)
project(codis-cpp VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
add_subdirectory(libs/cli)
add_subdirectory(libs/server)
