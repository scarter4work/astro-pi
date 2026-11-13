# ac_core: the PCL-free numerics/IO library (static, -fPIC so the module links it).
find_package(Threads REQUIRED)

add_library(ac_core STATIC Json.cpp
   num/Percentile.cpp num/Histogram.cpp num/Convolve.cpp num/Gaussian.cpp num/Label.cpp
   PyFormat.cpp fingerprint/Starlet.cpp fingerprint/Energy.cpp fingerprint/BandLimit.cpp
   fingerprint/Color.cpp fingerprint/Metrics.cpp fingerprint/Palette.cpp fingerprint/Extract.cpp
   fingerprint/Emd.cpp fingerprint/Distance.cpp
   optimize/Actions.cpp optimize/Recipe.cpp optimize/Propose.cpp optimize/Guardrails.cpp)
target_include_directories(ac_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")   # "core/Image.h"
target_link_libraries(ac_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
set_target_properties(ac_core PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(ac_core PUBLIC cxx_std_20)
# numpy/scipy parity is bit-exact: never let the compiler fuse a*b+c into an FMA. PUBLIC, because header-only
# numerics (num/Interp.h, num/Sum.h) are compiled inside every consumer (tests, the module).
target_compile_options(ac_core PUBLIC -ffp-contract=off)
