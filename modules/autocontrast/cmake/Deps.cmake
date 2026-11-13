# Third-party dependencies of AutoContrast.

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

# SQLite: the repo's vendored amalgamation (third_party/sqlite, public domain), same flags as
# modules/pi-copilot: hidden visibility (no sqlite3_* symbol leaks out of the module), no extension
# loading. Built only when something links it (the fingerprint store, a later task).
get_filename_component(AC_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(AC_SQLITE_DIR "${AC_REPO_ROOT}/third_party/sqlite")
add_library(ac_sqlite3 STATIC EXCLUDE_FROM_ALL "${AC_SQLITE_DIR}/sqlite3.c")
set_target_properties(ac_sqlite3 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    LINKER_LANGUAGE C)
target_compile_definitions(ac_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_DQS=0)
target_compile_options(ac_sqlite3 PRIVATE -w)   # third-party code: its warnings are not ours to fix
target_include_directories(ac_sqlite3 PUBLIC "${AC_SQLITE_DIR}")
find_package(Threads REQUIRED)
target_link_libraries(ac_sqlite3 PUBLIC Threads::Threads m)
