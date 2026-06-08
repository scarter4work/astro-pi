# Catch2 amalgamated — compile the source file once as a static library
add_library(catch2_main STATIC
    "${CMAKE_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp"
)
target_include_directories(catch2_main PUBLIC
    "${CMAKE_SOURCE_DIR}/third_party/catch2"
)

# Helper function to add a test executable.
#
# `--allow-running-no-tests` is passed so that test binaries whose
# only cases are opt-in (Catch2 dot-prefix tags like [.diag],
# [.integration], [.sweep], [.visual]) don't make ctest report them
# as failed when invoked without a tag filter.  The default Catch2
# behaviour is to exit non-zero when zero tests are selected, which
# ctest interprets as a failure.
function(nukex_add_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE catch2_main ${ARGN})
    target_include_directories(${test_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/catch2"
    )
    add_test(NAME ${test_name}
             COMMAND ${test_name} --allow-running-no-tests
             WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
endfunction()

# Core library tests — each added when implemented (Tasks 4-10)
nukex_add_test(test_image         unit/io/test_image.cpp           nukex4_io)
nukex_add_test(test_fits_reader   unit/io/test_fits_reader.cpp     nukex4_io)
nukex_add_test(test_debayer       unit/io/test_debayer.cpp         nukex4_io)
nukex_add_test(test_welford       unit/core/test_welford.cpp       nukex4_core)
nukex_add_test(test_histogram     unit/core/test_histogram.cpp     nukex4_core)
nukex_add_test(test_reservoir     unit/core/test_reservoir.cpp     nukex4_core)
nukex_add_test(test_distribution  unit/core/test_distribution.cpp  nukex4_core)
nukex_add_test(test_channel       unit/core/test_channel_config.cpp nukex4_core)
nukex_add_test(test_voxel         unit/core/test_voxel.cpp          nukex4_core)
nukex_add_test(test_cube          unit/core/test_cube.cpp           nukex4_core)
nukex_add_test(test_flat          unit/io/test_flat_calibration.cpp nukex4_io)
nukex_add_test(test_star_detector unit/alignment/test_star_detector.cpp nukex4_alignment)
nukex_add_test(test_homography     unit/alignment/test_homography.cpp     nukex4_alignment)
nukex_add_test(test_frame_aligner unit/alignment/test_frame_aligner.cpp nukex4_alignment)
nukex_add_test(test_alignment_diag unit/alignment/test_alignment_diag.cpp nukex4_alignment)
nukex_add_test(test_robust_stats  unit/fitting/test_robust_stats.cpp   nukex4_fitting)
nukex_add_test(test_student_t  unit/fitting/test_student_t_fitter.cpp  nukex4_fitting)
nukex_add_test(test_gmm        unit/fitting/test_gmm_fitter.cpp       nukex4_fitting)
nukex_add_test(test_contamination unit/fitting/test_contamination_fitter.cpp nukex4_fitting)
nukex_add_test(test_kde          unit/fitting/test_kde_fitter.cpp         nukex4_fitting)
nukex_add_test(test_model_selector unit/fitting/test_model_selector.cpp nukex4_fitting)
nukex_add_test(test_weight_computer unit/classify/test_weight_computer.cpp nukex4_classify)
nukex_add_test(test_pixel_selector unit/combine/test_pixel_selector.cpp nukex4_combine)
nukex_add_test(test_spatial_context unit/combine/test_spatial_context.cpp nukex4_combine)
nukex_add_test(test_output_assembler unit/combine/test_output_assembler.cpp nukex4_combine)
nukex_add_test(test_frame_cache unit/stacker/test_frame_cache.cpp nukex4_stacker)
nukex_add_test(test_stacking_engine unit/stacker/test_stacking_engine.cpp nukex4_stacker)

# Test utilities (PNG writer + test data loader)
add_library(test_util STATIC
    util/png_writer.cpp
    util/test_data_loader.cpp
)
target_include_directories(test_util PUBLIC
    "${CMAKE_SOURCE_DIR}/test/util"
    "${CMAKE_SOURCE_DIR}/third_party/stb"
    "${CMAKE_SOURCE_DIR}/third_party/catch2"
)
target_link_libraries(test_util PUBLIC nukex4_io nukex4_stretch)

nukex_add_test(test_stretch_pipeline unit/stretch/test_stretch_pipeline.cpp nukex4_stretch)
nukex_add_test(test_mtf unit/stretch/test_mtf.cpp nukex4_stretch test_util)
nukex_add_test(test_arcsinh unit/stretch/test_arcsinh.cpp nukex4_stretch test_util)
nukex_add_test(test_log unit/stretch/test_log.cpp nukex4_stretch test_util)
nukex_add_test(test_rnc unit/stretch/test_rnc.cpp nukex4_stretch test_util)
nukex_add_test(test_ghs unit/stretch/test_ghs.cpp nukex4_stretch test_util)
nukex_add_test(test_ots unit/stretch/test_ots.cpp nukex4_stretch test_util)
nukex_add_test(test_sas unit/stretch/test_sas.cpp nukex4_stretch test_util)
nukex_add_test(test_tier1 unit/stretch/test_tier1.cpp nukex4_stretch test_util)
nukex_add_test(test_calibrate unit/stretch/test_calibrate.cpp nukex4_stretch test_util)
nukex_add_test(test_param_bounds unit/stretch/test_param_bounds.cpp nukex4_stretch)
nukex_add_test(test_image_stats unit/stretch/test_image_stats.cpp nukex4_stretch test_util)
nukex_add_test(test_param_model unit/stretch/test_param_model.cpp nukex4_stretch test_util)
nukex_add_test(test_layer_loader unit/stretch/test_layer_loader.cpp nukex4_stretch test_util)

# Learning library tests (Phase 8 stats-driven tuning)
nukex_add_test(test_ridge_regression unit/learning/test_ridge_regression.cpp nukex4_learning)
nukex_add_test(test_rating_db unit/learning/test_rating_db.cpp nukex4_learning sqlite3_vendored)
nukex_add_test(test_train_model unit/learning/test_train_model.cpp nukex4_learning sqlite3_vendored)
nukex_add_test(test_atomic_write unit/learning/test_atomic_write.cpp nukex4_learning)

# GPU tests (link nukex4_stacker to resolve FrameCache symbols)
nukex_add_test(test_gpu_context unit/gpu/test_gpu_context.cpp nukex4_gpu)
nukex_add_test(test_gpu_cpu_fallback unit/gpu/test_gpu_cpu_fallback.cpp nukex4_gpu nukex4_stacker)
nukex_add_test(test_gpu_kernels unit/gpu/test_gpu_kernels.cpp nukex4_gpu)
nukex_add_test(test_gpu_agreement unit/gpu/test_gpu_agreement.cpp nukex4_gpu nukex4_stacker)
nukex_add_test(test_fit_heartbeat unit/gpu/test_fit_heartbeat.cpp nukex4_gpu)

# Module-layer tests (fits_metadata and future module code)
add_library(nukex4_module_testlib OBJECT
    "${CMAKE_SOURCE_DIR}/src/module/fits_metadata.cpp"
    "${CMAKE_SOURCE_DIR}/src/module/filter_classifier.cpp"
    "${CMAKE_SOURCE_DIR}/src/module/stretch_auto_selector.cpp"
    "${CMAKE_SOURCE_DIR}/src/module/stretch_factory.cpp"
)
target_include_directories(nukex4_module_testlib PUBLIC
    "${CMAKE_SOURCE_DIR}/src/module"
    "${CMAKE_SOURCE_DIR}/src/lib/io/include"
    "${CMAKE_SOURCE_DIR}/src/lib/stretch/include"
)
target_link_libraries(nukex4_module_testlib PUBLIC cfitsio nukex4_stretch)

nukex_add_test(test_fits_metadata unit/module/test_fits_metadata.cpp nukex4_module_testlib)
nukex_add_test(test_filter_classifier unit/module/test_filter_classifier.cpp nukex4_module_testlib)
nukex_add_test(test_stretch_auto_selector unit/module/test_stretch_auto_selector.cpp nukex4_module_testlib)
nukex_add_test(test_stretch_factory unit/module/test_stretch_factory.cpp nukex4_module_testlib)
nukex_add_test(test_phase8_fallback unit/module/test_phase8_fallback.cpp nukex4_stretch nukex4_learning sqlite3_vendored)
