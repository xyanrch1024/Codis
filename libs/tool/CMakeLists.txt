add_library(codis_tool STATIC
    src/tools/tools.cpp
)

target_include_directories(codis_tool PUBLIC src)
target_link_libraries(codis_tool PUBLIC
    nlohmann_json::nlohmann_json
    codis_protocol
    codis_util
)
