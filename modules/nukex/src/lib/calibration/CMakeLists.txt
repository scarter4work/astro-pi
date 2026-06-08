add_library(nukex4_calibration STATIC
    src/qe_database.cpp
    src/channel_decomposer.cpp
)

target_include_directories(nukex4_calibration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Eigen is PUBLIC because channel_decomposer.hpp exposes Eigen::MatrixXd /
# Eigen::Vector3d in its public API. Downstream consumers of nukex4_calibration
# (Phase B Q-solve in stacker) inherit Eigen via this transitive include.
target_link_libraries(nukex4_calibration
    PUBLIC  nukex4_core nukex4_io Eigen3::Eigen
    PRIVATE nlohmann_json::nlohmann_json
)

target_compile_features(nukex4_calibration PUBLIC cxx_std_17)
