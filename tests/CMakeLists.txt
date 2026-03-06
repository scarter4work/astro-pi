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

# QualityWeights unit tests
add_executable(test_quality_weights
    unit/test_quality_weights.cpp
    ${CMAKE_SOURCE_DIR}/src/engine/QualityWeights.cpp
)
target_link_libraries(test_quality_weights PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_quality_weights PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/xtensor/include
    ${CMAKE_SOURCE_DIR}/third_party/xtl/include
    ${CMAKE_SOURCE_DIR}/third_party/xsimd/include
)
target_compile_definitions(test_quality_weights PRIVATE XTENSOR_USE_XSIMD)
target_compile_features(test_quality_weights PRIVATE cxx_std_17)
add_test(NAME test_quality_weights COMMAND test_quality_weights)

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
