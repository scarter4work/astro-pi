cmake_minimum_required(VERSION 3.24)
project(GaiaDepthGrade VERSION 0.1.0 LANGUAGES C CXX)

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

option(GDG_BUILD_MODULE "Build the PixInsight module (requires PCL SDK)" OFF)
option(GDG_BUILD_TESTS "Build unit tests" ON)

# gdg_core: PCL-free numerics (always built; the module and the tests link it).
add_subdirectory(src/core)

if(GDG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
