add_library(nukex4_stacker STATIC
    src/frame_cache.cpp
    src/stacking_engine.cpp
)

target_include_directories(nukex4_stacker
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_stacker
    PUBLIC nukex4_core nukex4_io nukex4_alignment nukex4_fitting
           nukex4_classify nukex4_combine nukex4_gpu
)

target_compile_definitions(nukex4_stacker PRIVATE _FILE_OFFSET_BITS=64)

target_compile_features(nukex4_stacker PUBLIC cxx_std_17)
