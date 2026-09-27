cmake_minimum_required(VERSION 3.24)
project(PICopilot VERSION 0.1.0 LANGUAGES C CXX)

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

# nlohmann/json — header-only, vendored via FetchContent (verbatim shape
# from modules/nukex/CMakeLists.txt). Used for Anthropic API request/
# response bodies in later increment-2 tasks.
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# SQLite — VENDORED amalgamation (repo-root third_party/sqlite, public domain),
# compiled into the module: hidden visibility so no sqlite3_* symbol leaks out
# of PICopilot-pxm.so, no extension loading (no dlopen, keeps -Wl,-z,defs
# happy), foreign keys on by default. Used by the image-journey library.
get_filename_component(PICOPILOT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PICOPILOT_SQLITE_DIR "${PICOPILOT_REPO_ROOT}/third_party/sqlite")
add_library(picopilot_sqlite3 STATIC "${PICOPILOT_SQLITE_DIR}/sqlite3.c")
set_target_properties(picopilot_sqlite3 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    LINKER_LANGUAGE C)
target_compile_definitions(picopilot_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DQS=0)
target_compile_options(picopilot_sqlite3 PRIVATE -w)   # third-party code: its warnings are not ours to fix
target_include_directories(picopilot_sqlite3 PUBLIC "${PICOPILOT_SQLITE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(picopilot_sqlite3 PUBLIC Threads::Threads m)

add_subdirectory(src/module)
