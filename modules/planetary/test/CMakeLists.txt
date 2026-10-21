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
planetary_add_test(test_frame_source unit/test_frame_source.cpp planetary_ser)
planetary_add_test(test_avi unit/test_avi.cpp planetary_ser)
planetary_add_test(test_sidecar unit/test_sidecar.cpp planetary_ser)
planetary_add_test(test_ser_export unit/test_ser_export.cpp planetary_ser)
planetary_add_test(test_demosaic_hq unit/test_demosaic_hq.cpp planetary_stack)
planetary_add_test(test_measure unit/test_measure.cpp planetary_stack)
# FitsText.h is the module's PCL-free keyword-text helper (header only).
planetary_add_test(test_fits_text unit/test_fits_text.cpp)
target_include_directories(test_fits_text PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")
# PreviewPixels.h / SerInfoText.h: the preview's PCL-free pixel math and info text (header only).
planetary_add_test(test_preview unit/test_preview.cpp planetary_ser)
target_include_directories(test_preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")

# Test-only fixture writer for the headless PixInsight probe (test/run-probe.sh).
add_executable(make-fixtures make-fixtures.cpp)
target_include_directories(make-fixtures PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")
add_test(NAME make_fixtures_runs
         COMMAND make-fixtures "${CMAKE_CURRENT_BINARY_DIR}/fixtures-smoke")

# Test-only: checks the reader against the Saturn fixtures' independent expectations
# (test/run-real-data.sh runs it; not a ctest case, it needs the NAS capture).
add_executable(check-real-fixtures check-real-fixtures.cpp)
target_link_libraries(check-real-fixtures PRIVATE planetary_ser)

# Test-only: the real-data AVI proof's CLI over the reader (test/run-avi-real.sh runs it;
# not a ctest case, it needs the NAS captures).
add_executable(avi-probe avi-probe.cpp)
target_link_libraries(avi-probe PRIVATE planetary_ser)
