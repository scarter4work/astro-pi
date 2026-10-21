# PCL-free planetary stack engine (lucky imaging), built on the reader library
add_library(planetary_stack STATIC Demosaic.cpp)
target_include_directories(planetary_stack PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(planetary_stack PUBLIC planetary_ser)
set_target_properties(planetary_stack PROPERTIES POSITION_INDEPENDENT_CODE ON)
