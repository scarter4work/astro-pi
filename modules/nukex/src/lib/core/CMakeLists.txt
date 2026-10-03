add_library(nukex4_core STATIC
    src/channel_config.cpp
    src/cube.cpp
    src/coverage_trim.cpp
    src/filter.cpp
    src/noise_model.cpp
    src/fits_time.cpp
)

target_include_directories(nukex4_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(nukex4_core PUBLIC cxx_std_17)
