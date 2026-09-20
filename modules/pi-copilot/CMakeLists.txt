cmake_minimum_required(VERSION 3.24)
project(PICopilot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PICOPILOT_PLATFORM "linux")
    add_compile_definitions(__PCL_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PICOPILOT_PLATFORM "macos")
    add_compile_definitions(__PCL_MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PICOPILOT_PLATFORM "windows")
    add_compile_definitions(__PCL_WINDOWS)
endif()

add_compile_definitions(
    __PCL_BUILDING_MODULE
    _REENTRANT
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -fPIC
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -DNDEBUG)
    else()
        add_compile_options(-O0 -g)
    endif()
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/O2 /DNDEBUG)
    else()
        add_compile_options(/Od /Zi)
    endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(PICOPILOT_BUILD_MODULE "Build the PixInsight module (requires PCL SDK)" OFF)
if(PICOPILOT_BUILD_MODULE)
    include(PCLConfig)
endif()

add_subdirectory(src/module)
