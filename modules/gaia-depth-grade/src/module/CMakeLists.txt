# GaiaDepthGrade PixInsight module -> GaiaDepthGrade-pxm.so (Linux).
# PCLConfig (included by the top level) has already checked PCLDIR is PCL 2.13.

set(MODULE_SOURCES
    GaiaDepthGradeModule.cpp
    PclProbe.cpp
)

add_library(GaiaDepthGrade SHARED ${MODULE_SOURCES})

target_include_directories(GaiaDepthGrade PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# PCL headers are SYSTEM includes so warnings inside PCL don't flood the build.
target_include_directories(GaiaDepthGrade SYSTEM PRIVATE ${PCL_INCLUDE_DIRS})

target_compile_definitions(GaiaDepthGrade PRIVATE __PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT)
target_compile_options(GaiaDepthGrade PRIVATE -fPIC -fvisibility=hidden -fvisibility-inlines-hidden)

# Order matters for static archives: libPCL first, then what it uses.
target_link_libraries(GaiaDepthGrade PRIVATE ${PCL_LIBRARIES})

set_target_properties(GaiaDepthGrade PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "GaiaDepthGrade-pxm"
    LINK_FLAGS "-shared -Wl,-z,defs -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map"
)
target_compile_features(GaiaDepthGrade PRIVATE cxx_std_20)
set_property(TARGET GaiaDepthGrade APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map)
