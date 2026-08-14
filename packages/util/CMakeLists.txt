add_library(codis_util STATIC
    src/config.cpp
)

target_include_directories(codis_util PUBLIC src)
target_link_libraries(codis_util PUBLIC tomlplusplus::tomlplusplus)
