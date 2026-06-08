add_library(nukex4_combine STATIC
    src/pixel_selector.cpp
    src/spatial_context.cpp
    src/output_assembler.cpp
)

target_include_directories(nukex4_combine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_combine
    PUBLIC nukex4_core nukex4_fitting nukex4_classify nukex4_io
)

target_compile_features(nukex4_combine PUBLIC cxx_std_17)
