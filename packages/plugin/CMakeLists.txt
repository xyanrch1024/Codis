add_library(codis_plugin STATIC
    src/plugin_loader.cpp
)

target_include_directories(codis_plugin PUBLIC
    include
)
target_link_libraries(codis_plugin PUBLIC
    nlohmann_json::nlohmann_json
    codis_util
    ${CMAKE_DL_LIBS}
)
