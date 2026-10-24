# Planetary PixInsight Module
# Builds Planetary-pxm.so (Linux)

set(PCLDIR "$ENV{HOME}/PCL" CACHE PATH "PixInsight Class Library directory")

if(NOT EXISTS "${PCLDIR}/include/pcl/MetaModule.h")
    message(STATUS "PCL not found at ${PCLDIR} -- skipping module build")
    return()
endif()

message(STATUS "PCL found at ${PCLDIR} -- building Planetary module")

set(MODULE_SOURCES
    PlanetaryModule.cpp
    SERViewerProcess.cpp
    SERViewerParameters.cpp
    SERViewerInstance.cpp
    SERViewerInterface.cpp
    FramePreview.cpp
    BayerPatternParameter.cpp
    NewImageWindow.cpp
    PlanetaryStackProcess.cpp
    PlanetaryStackParameters.cpp
    PlanetaryStackInstance.cpp
    PlanetaryStackInterface.cpp
    VideoControls.cpp
)

add_library(Planetary SHARED ${MODULE_SOURCES})

# PCL headers are SYSTEM includes so warnings originating inside PCL don't
# flood the build; our own code is still compiled with -Wall -Wextra.
target_include_directories(Planetary PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(Planetary SYSTEM PRIVATE
    ${PCLDIR}/include
)

target_compile_definitions(Planetary PRIVATE
    __PCL_LINUX
    __PCL_BUILDING_MODULE
    _REENTRANT
)

target_compile_options(Planetary PRIVATE
    -fPIC
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)

target_link_directories(Planetary PRIVATE ${PCLDIR}/lib/x64)
target_link_libraries(Planetary PRIVATE
    planetary_stack
    planetary_ser
    PCL-pxi
    lz4-pxi
    zstd-pxi
    zlib-pxi
    RFC6234-pxi
    lcms-pxi
    cminpack-pxi
)

set_target_properties(Planetary PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "Planetary-pxm"
    LINK_FLAGS "-shared -Wl,-z,defs -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map -L${PCLDIR}/lib/x64"
)

target_compile_features(Planetary PRIVATE cxx_std_17)

# Relink when the export map changes.
set_property(TARGET Planetary APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map)
