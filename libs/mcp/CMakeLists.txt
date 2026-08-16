add_library(codis_mcp STATIC
    src/mcp_client.cpp
    src/mcp_manager.cpp
)

target_include_directories(codis_mcp PUBLIC src)
target_link_libraries(codis_mcp PUBLIC
    nlohmann_json::nlohmann_json
    httplib::httplib
    codis_tool
    codis_util
)