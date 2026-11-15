# A standalone consumer of NukeX's io library: add_subdirectory() of
# src/lib/core and src/lib/io with NONE of the NukeX root CMakeLists.txt's
# global definitions -- the way an embedding application (the Rust engine
# bridge) builds it. Driven by run.cmake from the test_fits_concurrent_standalone
# ctest; the sources come from the parent build (no network).
cmake_minimum_required(VERSION 3.24)
project(nukex_standalone_io LANGUAGES C CXX)

if(NOT NUKEX_ROOT)
    message(FATAL_ERROR "Pass -DNUKEX_ROOT=<astro-pi>/modules/nukex")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_subdirectory(${NUKEX_ROOT}/src/lib/core nukex/core)
add_subdirectory(${NUKEX_ROOT}/src/lib/io   nukex/io)

add_executable(test_fits_concurrent_standalone
    ${NUKEX_ROOT}/test/unit/io/test_fits_concurrent.cpp
    ${NUKEX_ROOT}/third_party/catch2/catch_amalgamated.cpp)
target_include_directories(test_fits_concurrent_standalone PRIVATE
    ${NUKEX_ROOT}/third_party/catch2)
target_link_libraries(test_fits_concurrent_standalone PRIVATE nukex4_io cfitsio)
