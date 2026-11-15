add_library(nukex4_core STATIC
    src/channel_config.cpp
    src/cube.cpp
    src/coverage_trim.cpp
    src/filter.cpp
    src/noise_model.cpp
    src/fits_time.cpp
    src/run_temp_dir.cpp
)

target_include_directories(nukex4_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(nukex4_core PUBLIC cxx_std_20)

# nukex/core/parallel_for.hpp (header-only, std::thread) is the project's
# replacement for OpenMP; every library that includes it gets Threads here.
find_package(Threads REQUIRED)
target_link_libraries(nukex4_core PUBLIC Threads::Threads)
