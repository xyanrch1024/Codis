add_executable(codis-server
    src/main.cpp
    src/server.cpp
    src/session_store.cpp
    src/context_source.cpp
)

target_include_directories(codis-server PRIVATE
    ${CMAKE_SOURCE_DIR}/libs/llm/src
    ${CMAKE_SOURCE_DIR}/libs/util/src
    ${CMAKE_SOURCE_DIR}/libs/plugin/include
)

target_link_libraries(codis-server PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    codis_llm
    codis_protocol
    codis_util
    codis_tool
    codis_plugin
)
