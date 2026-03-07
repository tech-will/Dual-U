project(CMakeLists.txt)

cmake_minimum_required(VERSION 3.10)

set(PROJECT_NAME wiiu_dual_drc_plugin)
set(SOURCE_DIR src)

include_directories(${SOURCE_DIR}/pairing)
include_directories(${SOURCE_DIR}/plugin)

file(GLOB_RECURSE SOURCES
    ${SOURCE_DIR}/pairing/*.cpp
    ${SOURCE_DIR}/plugin/*.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

set_target_properties(${PROJECT_NAME} PROPERTIES
    VERSION 1.0
    SOVERSION 1
)

install(TARGETS ${PROJECT_NAME} DESTINATION lib)
install(FILES ${SOURCE_DIR}/pairing/DrcPairing.hpp ${SOURCE_DIR}/pairing/Utils.hpp
        DESTINATION include/${PROJECT_NAME})