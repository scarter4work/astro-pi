nukex_add_test(test_phase_a_router test_phase_a_router.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core test_util)
target_compile_definitions(test_phase_a_router PRIVATE NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")

nukex_add_test(test_phase_b_qsolve test_phase_b_qsolve.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core test_util)

nukex_add_test(test_lrgb_mono test_lrgb_mono.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core test_util)
target_compile_definitions(test_lrgb_mono PRIVATE NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")

nukex_add_test(test_file_backed_accumulate test_file_backed_accumulate.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core cfitsio)
target_compile_definitions(test_file_backed_accumulate PRIVATE NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")
