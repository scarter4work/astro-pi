# PCL-free SER + AVI reader library
add_library(planetary_ser STATIC SerFile.cpp AviFile.cpp OpenVideo.cpp SerTime.cpp Debayer.cpp AsiairSidecar.cpp)
target_include_directories(planetary_ser PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(planetary_ser PROPERTIES POSITION_INDEPENDENT_CODE ON)
