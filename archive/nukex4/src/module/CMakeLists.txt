# NukeX v4 PixInsight Module
# Builds NukeX-pxm.so (Linux), NukeX-pxm.dylib (macOS), NukeX-pxm.dll (Windows)

set(PCLDIR "$ENV{HOME}/PCL" CACHE PATH "PixInsight Class Library directory")

if(NOT EXISTS "${PCLDIR}/include/pcl/MetaModule.h")
    message(STATUS "PCL not found at ${PCLDIR} -- skipping module build")
    return()
endif()

message(STATUS "PCL found at ${PCLDIR} -- building NukeX module")

set(MODULE_SOURCES
    NukeXModule.cpp
    NukeXProcess.cpp
    NukeXInstance.cpp
    NukeXInterface.cpp
    NukeXParameters.cpp
    NukeXProgress.cpp
    RatingDialog.cpp
    fits_metadata.cpp
    filter_classifier.cpp
    stretch_auto_selector.cpp
    stretch_factory.cpp
)

add_library(NukeX-pxm SHARED ${MODULE_SOURCES})

# PCL include directories.
#
# PCL headers are treated as SYSTEM includes so GCC's -Wparentheses and
# other warnings originating inside PCL (e.g. Image.h:16807's
# `m_side > 0 == x > m_center`) don't flood every build.  Our own code
# is still compiled with -Wall -Wextra per the top-level CMakeLists.
target_include_directories(NukeX-pxm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(NukeX-pxm SYSTEM PRIVATE
    ${PCLDIR}/include
)

# PCL compile definitions
target_compile_definitions(NukeX-pxm PRIVATE
    __PCL_LINUX
    __PCL_BUILDING_MODULE
    _REENTRANT
)

# PCL compile flags
target_compile_options(NukeX-pxm PRIVATE
    -fPIC
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# cfitsio: fits_metadata.cpp in the module uses fitsio.h directly.
# cfitsio is vendored via FetchContent in src/lib/io — link the target here
# so the module also gets the include path and the static archive.
target_link_libraries(NukeX-pxm PRIVATE cfitsio)

# Link NukeX v4 libraries
target_link_libraries(NukeX-pxm PRIVATE
    nukex4_stacker
    nukex4_gpu
    nukex4_stretch
    nukex4_combine
    nukex4_fitting
    nukex4_classify
    nukex4_alignment
    nukex4_io
    nukex4_core
)

# Link PCL static libraries
target_link_directories(NukeX-pxm PRIVATE ${PCLDIR}/lib/x64)
target_link_libraries(NukeX-pxm PRIVATE
    PCL-pxi
    lz4-pxi
    zstd-pxi
    zlib-pxi
    RFC6234-pxi
    lcms-pxi
    cminpack-pxi
)

# Shared library flags
set_target_properties(NukeX-pxm PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "NukeX-pxm"
    LINK_FLAGS "-shared -Wl,-z,defs"
)

target_compile_features(NukeX-pxm PRIVATE cxx_std_17)
