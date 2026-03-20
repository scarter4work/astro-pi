# NukeX v3 Tests
# Uses Catch2 v3 from third_party/catch2

# Catch2 v3 requires building its main library
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/catch2 ${CMAKE_BINARY_DIR}/catch2)

# SubCube unit tests
add_executable(test_sub_cube unit/test_sub_cube.cpp)
target_link_libraries(test_sub_cube PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_sub_cube PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
)
target_compile_definitions(test_sub_cube PRIVATE XTENSOR_USE_XSIMD)
target_compile_features(test_sub_cube PRIVATE cxx_std_17)
add_test(NAME test_sub_cube COMMAND test_sub_cube)

# NumericalUtils unit tests
add_executable(test_numerical_utils unit/test_numerical_utils.cpp)
target_link_libraries(test_numerical_utils PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_numerical_utils PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_numerical_utils PRIVATE cxx_std_17)
add_test(NAME test_numerical_utils COMMAND test_numerical_utils)

# DistributionFitter unit tests
add_executable(test_distribution_fitter
    unit/test_distribution_fitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DistributionFitter.cpp
)
target_link_libraries(test_distribution_fitter PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_distribution_fitter PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
)
target_compile_definitions(test_distribution_fitter PRIVATE BOOST_MATH_STANDALONE=1)
target_compile_features(test_distribution_fitter PRIVATE cxx_std_17)
add_test(NAME test_distribution_fitter COMMAND test_distribution_fitter)

# SkewNormal fitter unit tests
add_executable(test_skew_normal_fitter
    unit/test_skew_normal_fitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/SkewNormalFitter.cpp
)
target_link_libraries(test_skew_normal_fitter PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_skew_normal_fitter PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/lbfgspp/include
)
target_compile_definitions(test_skew_normal_fitter PRIVATE BOOST_MATH_STANDALONE=1)
target_compile_features(test_skew_normal_fitter PRIVATE cxx_std_17)
add_test(NAME test_skew_normal_fitter COMMAND test_skew_normal_fitter)

# GaussianMixEM unit tests
add_executable(test_gaussian_mix_em
    unit/test_gaussian_mix_em.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/GaussianMixEM.cpp
)
target_link_libraries(test_gaussian_mix_em PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_gaussian_mix_em PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
)
target_compile_definitions(test_gaussian_mix_em PRIVATE BOOST_MATH_STANDALONE=1)
target_compile_features(test_gaussian_mix_em PRIVATE cxx_std_17)
add_test(NAME test_gaussian_mix_em COMMAND test_gaussian_mix_em)

# OutlierDetector unit tests
add_executable(test_outlier_detector
    unit/test_outlier_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OutlierDetector.cpp
)
target_link_libraries(test_outlier_detector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_outlier_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
)
target_compile_definitions(test_outlier_detector PRIVATE BOOST_MATH_STANDALONE=1)
target_compile_features(test_outlier_detector PRIVATE cxx_std_17)
add_test(NAME test_outlier_detector COMMAND test_outlier_detector)

# PixelSelector unit tests
add_executable(test_pixel_selector
    unit/test_pixel_selector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PixelSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DistributionFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/SkewNormalFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/GaussianMixEM.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OutlierDetector.cpp
)
target_link_libraries(test_pixel_selector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_pixel_selector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/lbfgspp/include
)
target_compile_definitions(test_pixel_selector PRIVATE XTENSOR_USE_XSIMD BOOST_MATH_STANDALONE=1)
target_compile_features(test_pixel_selector PRIVATE cxx_std_17)
add_test(NAME test_pixel_selector COMMAND test_pixel_selector)

# Adaptive model selection unit tests
add_executable(test_adaptive_models
    unit/test_adaptive_models.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PixelSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DistributionFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/SkewNormalFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/GaussianMixEM.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OutlierDetector.cpp
)
target_link_libraries(test_adaptive_models PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_adaptive_models PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/lbfgspp/include
)
target_compile_definitions(test_adaptive_models PRIVATE XTENSOR_USE_XSIMD BOOST_MATH_STANDALONE=1)
target_compile_features(test_adaptive_models PRIVATE cxx_std_17)
add_test(NAME test_adaptive_models COMMAND test_adaptive_models)

# CUDA vs CPU equivalence tests
set(CUDA_EQUIV_SOURCES
    unit/test_cuda_equivalence.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PixelSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DistributionFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/SkewNormalFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/GaussianMixEM.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OutlierDetector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/cuda/CudaRuntime.cpp
)
if(NUKEX_CUDA_LANGUAGE)
    list(APPEND CUDA_EQUIV_SOURCES ${CMAKE_SOURCE_DIR}/src/engine/cuda/CudaPixelSelector.cu)
endif()
add_executable(test_cuda_equivalence ${CUDA_EQUIV_SOURCES})
if(NUKEX_CUDA_LANGUAGE)
    set_target_properties(test_cuda_equivalence PROPERTIES
        CUDA_STANDARD 17
        CUDA_SEPARABLE_COMPILATION ON
    )
endif()
target_link_libraries(test_cuda_equivalence PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_cuda_equivalence PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/lbfgspp/include
)
target_compile_definitions(test_cuda_equivalence PRIVATE XTENSOR_USE_XSIMD BOOST_MATH_STANDALONE=1)
if(NUKEX_HAS_CUDA AND NUKEX_CUDA_LANGUAGE)
    target_compile_definitions(test_cuda_equivalence PRIVATE NUKEX_HAS_CUDA)
    target_link_libraries(test_cuda_equivalence PRIVATE CUDA::cudart)
    target_include_directories(test_cuda_equivalence PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
endif()
target_compile_features(test_cuda_equivalence PRIVATE cxx_std_17)
add_test(NAME test_cuda_equivalence COMMAND test_cuda_equivalence)

# Integration test — full pipeline
add_executable(test_full_pipeline
    integration/test_full_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PixelSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DistributionFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/SkewNormalFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/GaussianMixEM.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OutlierDetector.cpp
)
target_link_libraries(test_full_pipeline PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_full_pipeline PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/lbfgspp/include
)
target_compile_definitions(test_full_pipeline PRIVATE XTENSOR_USE_XSIMD BOOST_MATH_STANDALONE=1)
target_compile_features(test_full_pipeline PRIVATE cxx_std_17)
add_test(NAME test_full_pipeline COMMAND test_full_pipeline)

# StarDetector unit tests
add_executable(test_star_detector
    unit/test_star_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/StarDetector.cpp
)
target_link_libraries(test_star_detector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_star_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_star_detector PRIVATE cxx_std_17)
add_test(NAME test_star_detector COMMAND test_star_detector)

# TriangleMatcher unit tests
add_executable(test_triangle_matcher
    unit/test_triangle_matcher.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/TriangleMatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/StarDetector.cpp
)
target_link_libraries(test_triangle_matcher PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_triangle_matcher PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_triangle_matcher PRIVATE cxx_std_17)
add_test(NAME test_triangle_matcher COMMAND test_triangle_matcher)

# FrameAligner unit tests
add_executable(test_frame_aligner
    unit/test_frame_aligner.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/FrameAligner.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/TriangleMatcher.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/StarDetector.cpp
)
target_link_libraries(test_frame_aligner PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_frame_aligner PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
)
target_compile_definitions(test_frame_aligner PRIVATE XTENSOR_USE_XSIMD)
target_compile_features(test_frame_aligner PRIVATE cxx_std_17)
add_test(NAME test_frame_aligner COMMAND test_frame_aligner)

# AutoStretchSelector unit tests
add_executable(test_auto_stretch_selector
    unit/test_auto_stretch_selector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/AutoStretchSelector.cpp
)
target_link_libraries(test_auto_stretch_selector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_auto_stretch_selector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_auto_stretch_selector PRIVATE cxx_std_17)
add_test(NAME test_auto_stretch_selector COMMAND test_auto_stretch_selector)

# Stretch algorithm unit tests (all 11 algorithms)
add_executable(test_stretch_algorithms
    unit/test_stretch_algorithms.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/IStretchAlgorithm.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/MTFStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/ArcSinhStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/GHStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/HistogramStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/LogStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/LumptonStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/RNCStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/PhotometricStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/OTSStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/SASStretch.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms/VeraluxStretch.cpp
)
target_link_libraries(test_stretch_algorithms PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_stretch_algorithms PRIVATE
    ${PCL_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/engine
    ${CMAKE_SOURCE_DIR}/src/engine/algorithms
)
target_link_directories(test_stretch_algorithms PRIVATE ${PCL_LIB_DIR})
target_link_libraries(test_stretch_algorithms PRIVATE ${PCL_LIBRARIES} pthread)
target_compile_definitions(test_stretch_algorithms PRIVATE __PCL_LINUX _REENTRANT)
target_compile_features(test_stretch_algorithms PRIVATE cxx_std_17)
add_test(NAME test_stretch_algorithms COMMAND test_stretch_algorithms)

# CudaRemediation CPU-fallback unit tests
add_executable(test_cuda_remediation
    unit/test_cuda_remediation.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/PixelSelector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DistributionFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/SkewNormalFitter.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/GaussianMixEM.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/OutlierDetector.cpp
)
target_link_libraries(test_cuda_remediation PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_cuda_remediation PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_math/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_config/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_throw_exception/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_core/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_type_traits/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_static_assert/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_mp11/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_integer/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_lexical_cast/include
    ${CMAKE_SOURCE_DIR}/third_party/boost_predef/include
    ${CMAKE_SOURCE_DIR}/third_party/eigen
    ${CMAKE_SOURCE_DIR}/third_party/lbfgspp/include
)
target_compile_definitions(test_cuda_remediation PRIVATE XTENSOR_USE_XSIMD BOOST_MATH_STANDALONE=1)
target_compile_features(test_cuda_remediation PRIVATE cxx_std_17)
add_test(NAME test_cuda_remediation COMMAND test_cuda_remediation)

# ArtifactDetector unit tests
add_executable(test_artifact_detector
    unit/test_artifact_detector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/ArtifactDetector.cpp
)
target_link_libraries(test_artifact_detector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_artifact_detector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
)
target_compile_features(test_artifact_detector PRIVATE cxx_std_17)
add_test(NAME test_artifact_detector COMMAND test_artifact_detector)

# DustCorrector unit tests
add_executable(test_dust_corrector
    unit/test_dust_corrector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/DustCorrector.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/ArtifactDetector.cpp
)
target_link_libraries(test_dust_corrector PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_dust_corrector PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
)
target_compile_features(test_dust_corrector PRIVATE cxx_std_17)
target_compile_definitions(test_dust_corrector PRIVATE XTENSOR_USE_XSIMD)
add_test(NAME test_dust_corrector COMMAND test_dust_corrector)

# FlatCalibrator unit tests
add_executable(test_flat_calibrator
    unit/test_flat_calibrator.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/FlatCalibrator.cpp
)
target_link_libraries(test_flat_calibrator PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_flat_calibrator PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_features(test_flat_calibrator PRIVATE cxx_std_17)
add_test(NAME test_flat_calibrator COMMAND test_flat_calibrator)
