cmake_minimum_required(VERSION 3.24)
project(NukeX VERSION 4.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(NUKEX_PLATFORM "linux")
    add_compile_definitions(__PCL_LINUX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(NUKEX_PLATFORM "macos")
    add_compile_definitions(__PCL_MACOSX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(NUKEX_PLATFORM "windows")
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

option(NUKEX_BUILD_MODULE "Build the PixInsight module (requires PCL SDK)" OFF)
if(NUKEX_BUILD_MODULE)
    include(PCLConfig)
endif()

# Distribution guardrail. When ON, subprojects hard-fail if they detect a dep
# graph that won't load inside PixInsight (e.g. system cfitsio → libcurl →
# OPENSSL_3.2.0 symbols that PI's bundled libssl can't resolve).
option(NUKEX_RELEASE_BUILD "Distribution-ready build; hard-fails on deps that break PixInsight module loading" OFF)

option(NUKEX_PROFILING "Add -fno-omit-frame-pointer for perf flamegraph capture" OFF)
if(NUKEX_PROFILING)
    message(STATUS "NUKEX_PROFILING=ON: adding -fno-omit-frame-pointer")
    add_compile_options(-fno-omit-frame-pointer)
endif()

add_subdirectory(src/lib/core)
add_subdirectory(src/lib/io)
add_subdirectory(src/lib/alignment)
add_subdirectory(src/lib/fitting)
add_subdirectory(src/lib/classify)
add_subdirectory(src/lib/combine)
add_subdirectory(src/lib/gpu)
add_subdirectory(src/lib/stacker)
add_subdirectory(src/lib/stretch)
add_subdirectory(src/lib/learning)
add_subdirectory(src/module)

option(NUKEX_BUILD_TESTS "Build unit and integration tests" ON)
if(NUKEX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# Release packaging (`make sign`, `make package`).  Always wired — the
# NukeX-pxm target is always added by src/module/CMakeLists.txt in this
# project, regardless of NUKEX_BUILD_MODULE, so the targets are safe to
# include unconditionally.
include(${CMAKE_SOURCE_DIR}/cmake/PackageAndSign.cmake)

# ═══════════════════════════════════════════════════════════════════════════
# `make e2e` — runs tools/validate_e2e.js through PixInsight against the
# manifest at test/fixtures/e2e_manifest.json.  The currently installed
# NukeX-pxm.so in /opt/PixInsight/bin is what gets exercised, so make sure
# to install the dev build first (sign + copy).
#
# To refresh goldens instead of verifying:
#     make e2e-regen
# or
#     NUKEX_E2E_REGEN=1 make e2e
# ═══════════════════════════════════════════════════════════════════════════
add_custom_target(e2e
    COMMAND env NUKEX_BUILD_DIR=${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/tools/run_e2e.sh
    COMMENT "Running E2E validation harness against installed NukeX module"
    VERBATIM
)

add_custom_target(e2e-regen
    COMMAND env NUKEX_BUILD_DIR=${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/tools/run_e2e.sh regen
    COMMENT "Regenerating E2E golden fingerprints from current binary"
    VERBATIM
)
