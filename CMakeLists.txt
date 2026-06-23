cmake_minimum_required(VERSION 3.20)
project(opencode-cpp VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# 包管理 — 统一 vcpkg manifest 模式
# 所有第三方库由 vcpkg 管理，零 FetchContent
# ==============================================================================

find_package(asio CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_subdirectory(packages/util)
add_subdirectory(packages/llm)
add_subdirectory(packages/cli)
add_subdirectory(packages/server)
