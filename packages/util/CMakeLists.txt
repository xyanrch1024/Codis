add_library(opencode_util STATIC
    src/config.cpp
)

target_include_directories(opencode_util PUBLIC src)
target_link_libraries(opencode_util PUBLIC tomlplusplus::tomlplusplus)
