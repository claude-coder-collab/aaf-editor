cmake_minimum_required(VERSION 3.28)

project(aaf_editor VERSION 0.1.0 DESCRIPTION "AAF library and editor" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/ProjectOptions.cmake)

add_subdirectory(libs/common)
add_subdirectory(libs/cfb)
add_subdirectory(apps/aaftool)

if(AAF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(AAF_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()
