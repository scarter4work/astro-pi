cmake_minimum_required(VERSION 3.24)
project(EZTools VERSION 1.0.0 LANGUAGES C CXX)

# PCL 2.13 is C++20: its headers use <bit>, <compare> and concepts.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fPIC -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -DNDEBUG)
    else()
        add_compile_options(-O0 -g)
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(EZTOOLS_BUILD_MODULE "Build the PixInsight module (requires PCL 2.13)" OFF)
option(EZTOOLS_BUILD_TESTS "Build unit tests" ON)

# The PCL-free engine: built always (the unit tests and the module both link it).
add_subdirectory(src/hazekill)

if(EZTOOLS_BUILD_MODULE)
    include(PCLConfig)
    add_subdirectory(src/module)
endif()

if(EZTOOLS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
