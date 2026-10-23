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
planetary_add_test(test_noise unit/test_noise.cpp planetary_stack)
# FitsText.h is the module's PCL-free keyword-text helper (header only).
planetary_add_test(test_fits_text unit/test_fits_text.cpp)
target_include_directories(test_fits_text PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")
# PreviewPixels.h / SerInfoText.h: the preview's PCL-free pixel math and info text (header only).
planetary_add_test(test_preview unit/test_preview.cpp planetary_ser)
target_include_directories(test_preview PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")
# StackCards.h: PlanetaryStack's PCL-free keyword plan and result text (header only).
planetary_add_test(test_stack_cards unit/test_stack_cards.cpp planetary_stack)
target_include_directories(test_stack_cards PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/module")

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

# Test-only: Select's step-7 focus reading on a real capture (the in-focus survey and the defocus series
# behind kGrossBlurLimit and kBlurLimit; test/run-focus-survey.sh runs it). Not a ctest case, it needs the
# NAS captures.
add_executable(focus-survey focus-survey.cpp)
target_link_libraries(focus-survey PRIVATE planetary_stack)

# Test-only: the in-focus limb read directly off a stack of a real capture's sharpest half, and each frame's
# quality with and without the capture's noise model (Select.h step 7: why the in-focus reading doubled with
# Ruling P26). Not a ctest case, it needs the NAS captures.
add_executable(limb-probe limb-probe.cpp)
target_include_directories(limb-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")
target_link_libraries(limb-probe PRIVATE planetary_stack)

# Test-only: the calibration probes behind Ruling P26's measured figures (the noise model, qualitySigma, step 7's
# classifier and reading, the region gate), one calib-<name> each; usage at the top of each source. Not ctest
# cases: some read NAS captures, the rest render synthetic frames for minutes.
foreach(probe classrate gapinj inject injtiles lowsnr mdq moon nm2 nm4 nreal p27 passes purer2 q2steady qcal2 regsig
              modelerror seedsel shot skynoise snr synthrate temporal winnoise)
    add_executable(calib-${probe} calibration/${probe}.cpp)
    target_include_directories(calib-${probe} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit" "${CMAKE_CURRENT_SOURCE_DIR}"
                                                      "${CMAKE_CURRENT_SOURCE_DIR}/calibration")
    target_link_libraries(calib-${probe} PRIVATE planetary_stack)
endforeach()
