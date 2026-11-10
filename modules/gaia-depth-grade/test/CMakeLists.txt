# Catch2 amalgamated, shared with the NukeX module
set(GDG_CATCH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../nukex/third_party/catch2")
add_library(catch2_main STATIC "${GDG_CATCH2_DIR}/catch_amalgamated.cpp")
target_include_directories(catch2_main PUBLIC "${GDG_CATCH2_DIR}")

# Absolute path of the committed goldens, for tests that read them.
set(GDG_GOLDEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/golden")

function(gdg_add_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE catch2_main ${ARGN})
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/util" "${CMAKE_CURRENT_SOURCE_DIR}/unit")
    target_compile_definitions(${test_name} PRIVATE GDG_GOLDEN_DIR="${GDG_GOLDEN_DIR}")
    add_test(NAME ${test_name}
             COMMAND ${test_name} --allow-running-no-tests
             WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

gdg_add_test(test_npy unit/test_npy.cpp)
gdg_add_test(test_gaussian unit/test_gaussian.cpp gdg_core)
