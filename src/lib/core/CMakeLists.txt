add_library(nukex4_core STATIC
    src/channel_config.cpp
    src/cube.cpp
)

target_include_directories(nukex4_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(nukex4_core PUBLIC cxx_std_17)
