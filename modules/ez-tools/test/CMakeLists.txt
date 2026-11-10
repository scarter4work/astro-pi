# Catch2 amalgamated, shared with the NukeX module
set(EZTOOLS_CATCH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../nukex/third_party/catch2")
add_library(eztools_catch2_main STATIC "${EZTOOLS_CATCH2_DIR}/catch_amalgamated.cpp")
target_include_directories(eztools_catch2_main PUBLIC "${EZTOOLS_CATCH2_DIR}")

# eztools_add_test(<name> <source> [libs...]): one Catch2 executable per test file.
# Goldens are addressed through EZTOOLS_TEST_DIR (the source tree).
function(eztools_add_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE eztools_catch2_main ${ARGN})
    target_include_directories(${test_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/util"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_definitions(${test_name} PRIVATE EZTOOLS_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${test_name} COMMAND ${test_name} WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

eztools_add_test(test_hazekill unit/test_hazekill.cpp eztools_hazekill)
