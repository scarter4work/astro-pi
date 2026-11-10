# Catch2 amalgamated, shared with the NukeX module
set(GDG_CATCH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../nukex/third_party/catch2")
add_library(gdg_catch2_main STATIC "${GDG_CATCH2_DIR}/catch_amalgamated.cpp")
target_include_directories(gdg_catch2_main PUBLIC "${GDG_CATCH2_DIR}")

# gdg_add_test(<name> <source> [libs...]): one Catch2 executable per test file,
# run from the build directory (goldens are addressed relative to the source tree).
function(gdg_add_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE gdg_catch2_main ${ARGN})
    target_include_directories(${test_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/unit"
        "${CMAKE_CURRENT_SOURCE_DIR}/util"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_definitions(${test_name} PRIVATE
        GDG_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${test_name}
             COMMAND ${test_name} --allow-running-no-tests
             WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

# GaiaDepthGradeVersion.h is PCL-free (header only): the version release.sh reads.
gdg_add_test(test_version unit/test_version.cpp)
