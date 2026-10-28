# PCL-free planetary stack engine (lucky imaging), built on the reader library
find_package(Threads REQUIRED)
add_library(planetary_stack STATIC Accumulate.cpp Align.cpp Demosaic.cpp Engine.cpp Fft.cpp Measure.cpp Mineral.cpp Noise.cpp Regions.cpp Select.cpp)
target_include_directories(planetary_stack PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(planetary_stack PUBLIC planetary_ser Threads::Threads)
set_target_properties(planetary_stack PROPERTIES POSITION_INDEPENDENT_CODE ON)
