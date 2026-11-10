# GaiaDepthGrade PixInsight module -> GaiaDepthGrade-pxm.so (Linux).
# PCLConfig (included by the top level) has already checked PCLDIR is PCL 2.13.

set(MODULE_SOURCES
    GaiaDepthGradeModule.cpp
    PclProbe.cpp
    ImageBridge.cpp
    PclSky.cpp
    LocalGaia.cpp
    TapClient.cpp
)

# GDG_BUILD_PROBES (OFF; host test builds only, never the container/release build) adds the
# test-only GaiaDepthGradeProbe process the in-PI probes (test/run-probe.sh) drive.
option(GDG_BUILD_PROBES "Add the test-only GaiaDepthGradeProbe process (never ship)" OFF)
if(GDG_BUILD_PROBES)
    list(APPEND MODULE_SOURCES
        probe/ProbeProcess.cpp
        probe/ProbeSky.cpp
        probe/ProbeLocalGaia.cpp
        probe/ProbeOnline.cpp
    )
endif()

add_library(GaiaDepthGrade SHARED ${MODULE_SOURCES})

target_include_directories(GaiaDepthGrade PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(GDG_BUILD_PROBES)
    target_compile_definitions(GaiaDepthGrade PRIVATE GDG_PROBES)
    target_include_directories(GaiaDepthGrade PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../test/util")
endif()
# PCL headers are SYSTEM includes so warnings inside PCL don't flood the build.
target_include_directories(GaiaDepthGrade SYSTEM PRIVATE ${PCL_INCLUDE_DIRS})

target_compile_definitions(GaiaDepthGrade PRIVATE __PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT)
target_compile_options(GaiaDepthGrade PRIVATE -fPIC -fvisibility=hidden -fvisibility-inlines-hidden)

# Order matters for static archives: libPCL first, then what it uses.
target_link_libraries(GaiaDepthGrade PRIVATE gdg_core ${PCL_LIBRARIES})

set_target_properties(GaiaDepthGrade PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "GaiaDepthGrade-pxm"
    LINK_FLAGS "-shared -Wl,-z,defs -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map"
)
target_compile_features(GaiaDepthGrade PRIVATE cxx_std_20)
set_property(TARGET GaiaDepthGrade APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map)
