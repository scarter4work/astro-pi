find_package(Ceres REQUIRED)
find_package(Eigen3 REQUIRED)

add_library(nukex4_fitting STATIC
    src/robust_stats.cpp
    src/student_t_fitter.cpp
    src/gmm_fitter.cpp
    src/contamination_fitter.cpp
    src/kde_fitter.cpp
    src/model_selector.cpp
)

target_include_directories(nukex4_fitting
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_fitting
    PUBLIC nukex4_core
    PRIVATE Ceres::ceres Eigen3::Eigen
)

target_compile_features(nukex4_fitting PUBLIC cxx_std_17)
