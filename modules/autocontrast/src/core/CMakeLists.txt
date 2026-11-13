# ac_core: the PCL-free numerics/IO library (static, -fPIC so the module links it).
find_package(Threads REQUIRED)

add_library(ac_core STATIC Json.cpp)
target_include_directories(ac_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")   # "core/Image.h"
target_link_libraries(ac_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
set_target_properties(ac_core PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(ac_core PUBLIC cxx_std_20)
