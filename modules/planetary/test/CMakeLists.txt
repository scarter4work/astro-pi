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
planetary_add_test(test_select unit/test_select.cpp planetary_stack)
planetary_add_test(test_align unit/test_align.cpp planetary_stack)
planetary_add_test(test_regions unit/test_regions.cpp planetary_stack)
planetary_add_test(test_accumulate unit/test_accumulate.cpp planetary_stack)
planetary_add_test(test_engine unit/test_engine.cpp planetary_stack)
# FitsText.h is the module's PCL-free keyword-text helper (header only).
planetary_add_test(test_fits_text unit/test_fits_text.cpp)
target_include_directories(test_fits_text PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")
# PreviewPixels.h / SerInfoText.h: the preview's PCL-free pixel math and info text (header only).
planetary_add_test(test_preview unit/test_preview.cpp planetary_ser)
target_include_directories(test_preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")
# StackCards.h: PlanetaryStack's PCL-free keyword plan and result text (header only).
planetary_add_test(test_stack_cards unit/test_stack_cards.cpp planetary_stack)
target_include_directories(test_stack_cards PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")

# NAS-free check that avi_real_check.py's fixed-pattern readout check (4b) fails closed
# (synthetic data, numpy only). Without numpy it exits 77 and ctest shows it as Skipped, never
# as passed; without python3 it is not registered, and configure says so.
# The `python3` on PATH, as the test/*.sh scripts use (FindPython would prefer a newer-numbered
# python3.X elsewhere, e.g. one without numpy).
find_program(PLANETARY_PYTHON3 NAMES python3)
if(PLANETARY_PYTHON3)
    add_test(NAME fpn_verdict_fails_closed
             COMMAND "${PLANETARY_PYTHON3}" -I "${CMAKE_CURRENT_SOURCE_DIR}/test_fpn_verdict.py")
    set_tests_properties(fpn_verdict_fails_closed PROPERTIES SKIP_RETURN_CODE 77)
else()
    message(STATUS "planetary: python3 not found -- fpn_verdict_fails_closed is not registered")
endif()

# Test-only fixture writer for the headless PixInsight probes (test/run-probe.sh, test/run-probe-stack.sh).
# planetary_stack: the --stack fixtures are rendered with StackSynth.h (the engine tests' scenes);
# PlanetaryVersion.h: the version their HISTORY expectations name.
add_executable(make-fixtures make-fixtures.cpp)
target_include_directories(make-fixtures PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit" "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")
target_link_libraries(make-fixtures PRIVATE planetary_stack)
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

# Test-only: global alignment on real captures (test/run-align-real.sh runs it; not a ctest case,
# it needs the NAS captures).
add_executable(align-probe align-probe.cpp)
target_link_libraries(align-probe PRIVATE planetary_stack)

# Test-only: the region stage on real captures (lucky regions vs the global-only stack; timings).
# Not a ctest case, it needs the NAS captures.
add_executable(stack-region-probe stack-region-probe.cpp)
target_link_libraries(stack-region-probe PRIVATE planetary_stack)

# Test-only: RunStack on a real capture -- summary (JSON with --json), phase times, read rate, peak RSS,
# the stack as FITS or TIFF (Task 6's real-data smoke; Task 9's harness). Not a ctest case, it needs the
# NAS captures.
add_executable(stack-probe stack-probe.cpp)
target_link_libraries(stack-probe PRIVATE planetary_stack)
