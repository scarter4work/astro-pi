add_library(nukex4_classify STATIC
    src/weight_computer.cpp
)

target_include_directories(nukex4_classify
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_classify
    PUBLIC nukex4_core
)

target_compile_features(nukex4_classify PUBLIC cxx_std_17)
