add_executable(codis-server
    src/main.cpp
    src/server.cpp
)

target_include_directories(codis-server PRIVATE
    ${CMAKE_SOURCE_DIR}/packages/llm/src
    ${CMAKE_SOURCE_DIR}/packages/util/src
    ${CMAKE_SOURCE_DIR}/packages/plugin/include
)

target_link_libraries(codis-server PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    codis_llm
    codis_util
    codis_plugin
)
