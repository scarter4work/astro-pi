# gdg_core: the PCL-free numerics, in the Python core's frame and double precision.
# Built on the host for ctest without PixInsight, and linked into the module.
find_package(Threads REQUIRED)

add_library(gdg_core STATIC
    Numerics.cpp
    SigmaClip.cpp
    Morphology.cpp
    DaoFind.cpp
    Detect.cpp
    Transform.cpp
    HaloProfile.cpp
    RenderStars.cpp
    Nebula.cpp
    Display.cpp
)
# Consumers include "core/<Header>.h".
target_include_directories(gdg_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(gdg_core PUBLIC Threads::Threads)
# Parity with numpy/scipy needs their exact operation order: never contract a*b+c into an FMA
# (x86-64 baseline has none, but aarch64 and -march builds would). PUBLIC so header-inline
# numerics in consumers follow the same rule.
target_compile_options(gdg_core PUBLIC -ffp-contract=off)
