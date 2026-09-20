# PI Copilot PixInsight Module
# Builds PICopilot-pxm.so (Linux), PICopilot-pxm.dylib (macOS), PICopilot-pxm.dll (Windows)

set(PCLDIR "$ENV{HOME}/PCL" CACHE PATH "PixInsight Class Library directory")

if(NOT EXISTS "${PCLDIR}/include/pcl/MetaModule.h")
    message(STATUS "PCL not found at ${PCLDIR} -- skipping module build")
    return()
endif()

message(STATUS "PCL found at ${PCLDIR} -- building PICopilot module")

set(MODULE_SOURCES
    PICopilotModule.cpp
    PICopilotProcess.cpp
    PICopilotInstance.cpp
    PICopilotInterface.cpp
)

add_library(PICopilot SHARED ${MODULE_SOURCES})

# PCL include directories.
#
# PCL headers are treated as SYSTEM includes so GCC's -Wparentheses and
# other warnings originating inside PCL don't flood every build. Our own
# code is still compiled with -Wall -Wextra per the top-level CMakeLists.
target_include_directories(PICopilot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(PICopilot SYSTEM PRIVATE
    ${PCLDIR}/include
)

# PCL compile definitions
target_compile_definitions(PICopilot PRIVATE
    __PCL_LINUX
    __PCL_BUILDING_MODULE
    _REENTRANT
)

# PCL compile flags
target_compile_options(PICopilot PRIVATE
    -fPIC
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

# Link PCL static libraries
target_link_directories(PICopilot PRIVATE ${PCLDIR}/lib/x64)
target_link_libraries(PICopilot PRIVATE
    PCL-pxi
    lz4-pxi
    zstd-pxi
    zlib-pxi
    RFC6234-pxi
    lcms-pxi
    cminpack-pxi
)

# Shared library flags
set_target_properties(PICopilot PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "PICopilot-pxm"
    LINK_FLAGS "-shared -Wl,-z,defs -L${PCLDIR}/lib/x64"
)

target_compile_features(PICopilot PRIVATE cxx_std_17)
