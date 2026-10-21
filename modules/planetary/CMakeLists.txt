cmake_minimum_required(VERSION 3.24)
project(Planetary VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
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
elseif(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(PLANETARY_BUILD_MODULE "Build the PixInsight module (requires PCL SDK)" OFF)
option(PLANETARY_BUILD_TESTS "Build unit tests" ON)

if(PLANETARY_BUILD_MODULE)
    include(PCLConfig)
endif()

add_subdirectory(src/ser)
add_subdirectory(src/stack)

if(PLANETARY_BUILD_MODULE)
    add_subdirectory(src/module)
endif()

if(PLANETARY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
