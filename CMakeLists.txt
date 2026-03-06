# NukeX v3 - Statistical Stacking + Stretch for PixInsight
# Copyright (c) 2026 Scott Carter
# CMake build configuration

cmake_minimum_required(VERSION 3.15)
project(NukeX VERSION 3.0.0 LANGUAGES CXX)

# C++17 required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# PixInsight SDK
if(NOT DEFINED PCLDIR)
    if(DEFINED ENV{PCLDIR})
        set(PCLDIR $ENV{PCLDIR})
    else()
        message(FATAL_ERROR "PCLDIR not set. Please set PCLDIR environment variable or pass -DPCLDIR=...")
    endif()
endif()

# PCL include/lib paths
set(PCL_INCLUDE_DIR "${PCLDIR}/include")

# Platform-specific settings
if(WIN32)
    set(PCL_LIB_DIR "${PCLDIR}/lib/x64")
    set(PCL_LIBRARIES
        PCL-pxi lz4-pxi zstd-pxi zlib-pxi RFC6234-pxi lcms-pxi cminpack-pxi
    )
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    set(PCL_LIB_DIR "${PCLDIR}/lib/x64")
    set(PCL_LIBRARIES
        PCL-pxi lz4-pxi zstd-pxi zlib-pxi RFC6234-pxi lcms-pxi cminpack-pxi
    )
else()
    # Linux uses static libraries in lib/x64
    set(PCL_LIB_DIR "${PCLDIR}/lib/x64")
    set(PCL_LIBRARIES
        PCL-pxi lz4-pxi zstd-pxi zlib-pxi RFC6234-pxi lcms-pxi cminpack-pxi
    )
endif()

# OpenMP support for parallel processing
find_package(OpenMP)

# Source files — populated as code is added
file(GLOB_RECURSE NUKEX_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE NUKEX_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# Create shared library
add_library(NukeX SHARED ${NUKEX_SOURCES} ${NUKEX_HEADERS})

# Include directories
target_include_directories(NukeX PRIVATE
    ${PCL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/algorithms
    # xtensor stack
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/xsimd/include
    # LBFGSpp + Eigen
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lbfgspp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/eigen
    # Boost sub-repos (each has include/ with boost/ subfolder)
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost_predef/include
)

# xtensor SIMD acceleration
target_compile_definitions(NukeX PRIVATE XTENSOR_USE_XSIMD)

# Boost.Math standalone mode (avoids lexical_cast/container dependencies)
target_compile_definitions(NukeX PRIVATE BOOST_MATH_STANDALONE=1)

# Link directories and libraries
target_link_directories(NukeX PRIVATE ${PCL_LIB_DIR})
target_link_libraries(NukeX PRIVATE ${PCL_LIBRARIES} pthread)

# OpenMP support
if(OpenMP_CXX_FOUND)
    target_compile_options(NukeX PRIVATE ${OpenMP_CXX_FLAGS})
    target_link_libraries(NukeX PRIVATE OpenMP::OpenMP_CXX)
endif()

# Compiler flags (matching PCL build system)
if(MSVC)
    target_compile_options(NukeX PRIVATE /W4 /O2)
else()
    target_compile_options(NukeX PRIVATE
        -Wall -Wno-parentheses
        -O3 -fPIC
        -ffunction-sections -fdata-sections
        -fvisibility=hidden -fvisibility-inlines-hidden
        -fnon-call-exceptions
    )
    target_compile_definitions(NukeX PRIVATE _REENTRANT __PCL_BUILDING_MODULE)
    if(NOT APPLE)
        target_link_options(NukeX PRIVATE
            -Wl,-z,noexecstack
            -Wl,-O1
            -Wl,--gc-sections
            -rdynamic
        )
    endif()
endif()

# Platform definitions
if(WIN32)
    target_compile_definitions(NukeX PRIVATE __PCL_WINDOWS)
elseif(APPLE)
    target_compile_definitions(NukeX PRIVATE __PCL_MACOSX)
else()
    target_compile_definitions(NukeX PRIVATE __PCL_LINUX)
endif()

# Output configuration - PixInsight module naming convention
set_target_properties(NukeX PROPERTIES
    OUTPUT_NAME "NukeX-pxm"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Install
install(TARGETS NukeX
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# Copy icon
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icons/NukeX.svg")
    install(FILES icons/NukeX.svg DESTINATION share/NukeX/icons)
endif()

# Tests
enable_testing()
add_subdirectory(tests)

# Print configuration
message(STATUS "")
message(STATUS "NukeX v3 Configuration:")
message(STATUS "  Version:       ${PROJECT_VERSION}")
message(STATUS "  PCLDIR:        ${PCLDIR}")
message(STATUS "  C++ Standard:  17")
message(STATUS "  OpenMP:        ${OpenMP_CXX_FOUND}")
message(STATUS "")
