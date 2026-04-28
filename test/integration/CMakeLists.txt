nukex_add_test(test_phase_a_router test_phase_a_router.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core test_util)
target_compile_definitions(test_phase_a_router PRIVATE NUKEX_TEST_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/test/fixtures")

nukex_add_test(test_phase_b_qsolve test_phase_b_qsolve.cpp
    nukex4_stacker nukex4_calibration nukex4_compose nukex4_io nukex4_core test_util)
