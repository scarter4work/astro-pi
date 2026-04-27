add_library(nukex4_compose STATIC
    src/palette.cpp
)

target_include_directories(nukex4_compose
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_compose
    PUBLIC nukex4_core nukex4_io
)

target_compile_features(nukex4_compose PUBLIC cxx_std_17)
