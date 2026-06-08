find_package(Eigen3 REQUIRED NO_MODULE)

add_library(nukex4_alignment STATIC
    src/star_detector.cpp
    src/star_matcher.cpp
    src/homography.cpp
    src/frame_aligner.cpp
)

target_include_directories(nukex4_alignment
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_alignment
    PUBLIC nukex4_core nukex4_io
    PRIVATE Eigen3::Eigen
)

target_compile_features(nukex4_alignment PUBLIC cxx_std_17)
