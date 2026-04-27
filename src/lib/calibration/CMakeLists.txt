add_library(nukex4_calibration STATIC
    src/qe_database.cpp
)

target_include_directories(nukex4_calibration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(nukex4_calibration
    PUBLIC  nukex4_core nukex4_io
    PRIVATE nlohmann_json::nlohmann_json
)

target_compile_features(nukex4_calibration PUBLIC cxx_std_17)
