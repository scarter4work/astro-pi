# eztools_hazekill: the PCL-free EZHazeKill engine (unit-tested on its own, linked into the module).
add_library(eztools_hazekill STATIC HazeKill.cpp)
target_include_directories(eztools_hazekill PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(eztools_hazekill PUBLIC cxx_std_20)
