# NukeX v3 Tests
# Uses Catch2 v3 from third_party/catch2

# Catch2 v3 requires building its main library
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/catch2 ${CMAKE_BINARY_DIR}/catch2)

# Unit tests will be added here as source files are created
# Example:
# add_executable(nukex_tests unit/test_example.cpp)
# target_link_libraries(nukex_tests PRIVATE Catch2::Catch2WithMain)
# add_test(NAME nukex_tests COMMAND nukex_tests)
