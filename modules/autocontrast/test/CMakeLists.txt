# Catch2 amalgamated, shared with the NukeX module
set(AC_CATCH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../nukex/third_party/catch2")
add_library(ac_catch2_main STATIC "${AC_CATCH2_DIR}/catch_amalgamated.cpp")
target_include_directories(ac_catch2_main PUBLIC "${AC_CATCH2_DIR}")

# ac_add_test(<name> <source> [libs...]): one Catch2 executable per test file,
# run from the build directory (goldens are addressed relative to the source tree).
function(ac_add_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE ac_catch2_main nlohmann_json::nlohmann_json ${ARGN})
    target_include_directories(${test_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/util"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_definitions(${test_name} PRIVATE
        AC_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${test_name}
             COMMAND ${test_name} --allow-running-no-tests
             WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

ac_add_test(test_npy unit/test_npy.cpp)
ac_add_test(test_version  unit/test_version.cpp)
ac_add_test(test_parallel unit/test_parallel.cpp ac_core Threads::Threads)
ac_add_test(test_json_python unit/test_json_python.cpp ac_core)
ac_add_test(test_num unit/test_num.cpp ac_core)
ac_add_test(test_starlet unit/test_starlet.cpp ac_core)
ac_add_test(test_bandlimit unit/test_bandlimit.cpp ac_core)
