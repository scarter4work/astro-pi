# Compile the shipping QE database INTO the module binary. This removes the
# entire "loose share/qe_database.json not found at runtime" failure mode:
# there is no file to locate and no CWD assumption to get wrong. The generated
# TU defines nukex::embedded_qe_database_json(); it regenerates whenever the
# source JSON or the embedder script changes.
get_filename_component(_qe_json
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../share/qe_database.json" ABSOLUTE)
set(_qe_embedded_cpp "${CMAKE_CURRENT_BINARY_DIR}/qe_database_embedded.cpp")
add_custom_command(
    OUTPUT  "${_qe_embedded_cpp}"
    COMMAND "${CMAKE_COMMAND}"
            "-DQE_JSON_IN=${_qe_json}"
            "-DQE_CPP_OUT=${_qe_embedded_cpp}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_qe_database.cmake"
    DEPENDS "${_qe_json}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_qe_database.cmake"
    COMMENT "Embedding share/qe_database.json into NukeX binary"
    VERBATIM
)

add_library(nukex4_calibration STATIC
    src/qe_database.cpp
    src/channel_decomposer.cpp
    "${_qe_embedded_cpp}"
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
