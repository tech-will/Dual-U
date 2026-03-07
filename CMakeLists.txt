project(wiiu-dual-drc-plugin)

cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 11)

include_directories(include)

file(GLOB_RECURSE SOURCES
    src/pairing/*.cpp
    src/plugin/*.cpp
)

add_library(wiiu_dual_drc_plugin SHARED ${SOURCES})

set_target_properties(wiiu_dual_drc_plugin PROPERTIES
    OUTPUT_NAME "wiiu_dual_drc_plugin"
    PREFIX ""
)

target_link_libraries(wiiu_dual_drc_plugin
    # Add any necessary libraries here
)