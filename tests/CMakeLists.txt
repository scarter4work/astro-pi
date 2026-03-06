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
