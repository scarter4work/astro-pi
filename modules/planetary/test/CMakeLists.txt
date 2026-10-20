# Catch2 amalgamated, shared with the NukeX module
set(PLANETARY_CATCH2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../nukex/third_party/catch2")
add_library(catch2_main STATIC "${PLANETARY_CATCH2_DIR}/catch_amalgamated.cpp")
target_include_directories(catch2_main PUBLIC "${PLANETARY_CATCH2_DIR}")

function(planetary_add_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE catch2_main ${ARGN})
    target_include_directories(${test_name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")
    add_test(NAME ${test_name}
             COMMAND ${test_name} --allow-running-no-tests
             WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

planetary_add_test(test_ser_header unit/test_ser_header.cpp planetary_ser)
planetary_add_test(test_ser_frames unit/test_ser_frames.cpp planetary_ser)
planetary_add_test(test_debayer unit/test_debayer.cpp planetary_ser)
