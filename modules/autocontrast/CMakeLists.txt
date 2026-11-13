cmake_minimum_required(VERSION 3.24)
project(AutoContrast VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd: compile_commands.json lands in the build dir

# PCL 2.13 is C++20 (its headers use <bit>, <compare>, concepts).
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

option(AC_BUILD_TESTS  "Build the AutoContrast ctest suite" ON)
option(AC_BUILD_MODULE "Build the AutoContrast PCL module (requires PCL 2.13)" OFF)
option(AC_BUILD_PROBES "Compile the test-only AutoContrastProbe process into the module (never shipped)" OFF)

# PCLConfig before Deps: a module build links the raster decoders against PCL's zlib-pxi (cmake/Deps.cmake).
if(AC_BUILD_MODULE)
    include(PCLConfig)
endif()
include(Deps)
add_subdirectory(src/core)

if(AC_BUILD_MODULE)
    add_subdirectory(src/module)
elseif(AC_BUILD_PROBES)
    message(FATAL_ERROR "AC_BUILD_PROBES needs AC_BUILD_MODULE=ON")
endif()

if(AC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
