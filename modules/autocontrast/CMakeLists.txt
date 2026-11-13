cmake_minimum_required(VERSION 3.20)
project(AutoContrast LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AC_BUILD_TESTS  "Build the AutoContrast ctest suite" ON)
option(AC_BUILD_MODULE "Build the AutoContrast PCL module (not wired up yet)" OFF)

# nlohmann/json: header-only, same declaration as modules/pi-copilot/CMakeLists.txt.
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

if(AC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
