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
ac_add_test(test_fingerprint unit/test_fingerprint.cpp ac_core)
ac_add_test(test_palette unit/test_palette.cpp ac_core)
ac_add_test(test_emd unit/test_emd.cpp ac_core)
ac_add_test(test_distance unit/test_distance.cpp ac_core)
ac_add_test(test_actions unit/test_actions.cpp ac_core)
ac_add_test(test_propose unit/test_propose.cpp ac_core)
ac_add_test(test_guardrails unit/test_guardrails.cpp ac_core)
ac_add_test(test_loop unit/test_loop.cpp ac_core_approx)
ac_add_test(test_session unit/test_session.cpp ac_core_approx)
ac_add_test(test_resample unit/test_resample.cpp ac_core)
ac_add_test(test_raster unit/test_raster.cpp ac_core)

# Real-render tests (tools/autocontrast/data, or env AC_DATA_DIR): their own Catch2 main exits 77 -- ctest SKIP --
# only when the corpus is absent (util/DataDir.h); a present corpus with a missing or changed file fails.
add_library(ac_catch2_nomain STATIC "${AC_CATCH2_DIR}/catch_amalgamated.cpp")
target_include_directories(ac_catch2_nomain PUBLIC "${AC_CATCH2_DIR}")
target_compile_definitions(ac_catch2_nomain PUBLIC CATCH_AMALGAMATED_CUSTOM_MAIN)

function(ac_add_data_exe exe_name exe_source)
    add_executable(${exe_name} ${exe_source} util/data_main.cpp)
    target_link_libraries(${exe_name} PRIVATE ac_catch2_nomain nlohmann_json::nlohmann_json ${ARGN})
    target_include_directories(${exe_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/unit" "${CMAKE_CURRENT_SOURCE_DIR}/util" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_definitions(${exe_name} PRIVATE AC_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()

# ac_add_data_test(<ctest name> <exe> [catch2 test spec]): one ctest entry, so the slow real-data cases run in
# parallel under ctest -j.
function(ac_add_data_test test_name exe_name)
    add_test(NAME ${test_name} COMMAND ${exe_name} ${ARGN} WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

ac_add_data_exe(test_raster_real unit/test_raster_real.cpp ac_core)
ac_add_data_test(test_raster_real_decode test_raster_real jpeg_decode_bitexact_vs_pillow)
ac_add_data_test(test_raster_real_load test_raster_real load_raster_matches_python)
ac_add_data_exe(test_extract_real unit/test_extract_real.cpp ac_core)
ac_add_data_test(test_extract_real test_extract_real)
# own process: getrusage(ru_maxrss) must see only this load
ac_add_data_exe(test_raster_perf unit/test_raster_perf.cpp ac_core)
ac_add_data_test(test_raster_perf test_raster_perf)
