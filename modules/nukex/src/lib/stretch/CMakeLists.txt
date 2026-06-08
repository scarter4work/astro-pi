add_library(nukex4_stretch STATIC
    src/stretch_pipeline.cpp
    src/stretch_utils.cpp
    src/image_stats.cpp
    src/param_model.cpp
    src/layer_loader.cpp
    src/mtf_stretch.cpp
    src/arcsinh_stretch.cpp
    src/log_stretch.cpp
    src/rnc_stretch.cpp
    src/ghs_stretch.cpp
    src/ots_stretch.cpp
    src/sas_stretch.cpp
    src/lupton_stretch.cpp
    src/veralux_stretch.cpp
    src/clahe_stretch.cpp
    src/photometric_stretch.cpp
)

target_include_directories(nukex4_stretch
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_stretch
    PUBLIC nukex4_core nukex4_io nukex4_learning
)

target_compile_features(nukex4_stretch PUBLIC cxx_std_17)
