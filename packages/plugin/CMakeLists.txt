add_library(opencode_plugin STATIC
    src/plugin_loader.cpp
)

target_include_directories(opencode_plugin PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/packages/llm/src    # log.h
)
target_link_libraries(opencode_plugin PUBLIC
    nlohmann_json::nlohmann_json
    ${CMAKE_DL_LIBS}
)
