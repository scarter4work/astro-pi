cmake_minimum_required(VERSION 3.24)
project(GaiaDepthGrade VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd: compile_commands.json lands in the build dir

# PCL 2.13 (the only PCL with BlindAstrometricSolver) is C++20: its headers use
# <bit>, <compare> and concepts, and its makefiles build with -std=c++20.
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

option(GDG_BUILD_MODULE "Build the PixInsight module (requires PCL 2.13)" OFF)
option(GDG_BUILD_TESTS "Build unit tests" ON)

if(GDG_BUILD_MODULE)
    include(PCLConfig)
    add_subdirectory(src/module)
endif()

if(GDG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
