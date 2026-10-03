# Compile the shipping QE database INTO the module binary. This removes the
# entire "loose share/qe_database.json not found at runtime" failure mode:
# there is no file to locate and no CWD assumption to get wrong. The generated
# TU defines nukex::embedded_qe_database_json() and
# embedded_qe_database_version() (the db_version of the signed publication in
# repository/qe_manifest.json that matches these bytes); it regenerates
# whenever the JSON, the manifest or the embedder script changes. A database
# downloaded by the in-module updater (QEUpdater) is a file under the user's
# config directory and takes precedence only while it is newer.
get_filename_component(_qe_json
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../share/qe_database.json" ABSOLUTE)
get_filename_component(_qe_manifest
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../repository/qe_manifest.json" ABSOLUTE)
set(_qe_embedded_cpp "${CMAKE_CURRENT_BINARY_DIR}/qe_database_embedded.cpp")
add_custom_command(
    OUTPUT  "${_qe_embedded_cpp}"
    COMMAND "${CMAKE_COMMAND}"
            "-DQE_JSON_IN=${_qe_json}"
            "-DQE_MANIFEST_IN=${_qe_manifest}"
            "-DQE_CPP_OUT=${_qe_embedded_cpp}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_qe_database.cmake"
    DEPENDS "${_qe_json}" "${_qe_manifest}" "${CMAKE_CURRENT_SOURCE_DIR}/embed_qe_database.cmake"
    COMMENT "Embedding share/qe_database.json into NukeX binary"
    VERBATIM
)

add_library(nukex4_calibration STATIC
    src/qe_database.cpp
    src/channel_decomposer.cpp
    src/background_neutralization.cpp
    src/background_gradient.cpp
    src/frame_normalization.cpp
    src/ed25519_verify.cpp
    src/qe_update.cpp
    src/qe_update_state.cpp
    "${_qe_embedded_cpp}"
    ${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c
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

# TweetNaCl is PRIVATE: an implementation detail of ed25519_verify.cpp, with
# no public header exposing its types.
target_include_directories(nukex4_calibration
    PRIVATE ${CMAKE_SOURCE_DIR}/third_party/tweetnacl
)

# Vendored C89 that trips the project's warning flags inside its own bignum
# code. Silence that one translation unit rather than lowering the bar
# project-wide, and keep the upstream file byte-identical.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/third_party/tweetnacl/tweetnacl.c
    PROPERTIES COMPILE_OPTIONS "-w"
)

target_compile_features(nukex4_calibration PUBLIC cxx_std_17)
