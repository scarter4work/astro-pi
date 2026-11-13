# AutoContrast PixInsight module -> AutoContrast-pxm.so (Linux).
# PCLConfig (included by the top level) has already checked PCLDIR is PCL 2.13.

set(MODULE_SOURCES
    AutoContrastModule.cpp
    AutoContrastProcess.cpp
    AutoContrastReferencesProcess.cpp
)
if(AC_BUILD_PROBES)
    list(APPEND MODULE_SOURCES
        probe/AutoContrastProbeProcess.cpp
        probe/AutoContrastProbeInstance.cpp)
endif()

add_library(AutoContrast SHARED ${MODULE_SOURCES})

target_include_directories(AutoContrast PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# PCL headers are SYSTEM includes so warnings inside PCL don't flood the build.
target_include_directories(AutoContrast SYSTEM PRIVATE ${PCL_INCLUDE_DIRS})

target_compile_definitions(AutoContrast PRIVATE __PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT)
if(AC_BUILD_PROBES)
    target_compile_definitions(AutoContrast PRIVATE AC_BUILD_PROBES)
endif()
target_compile_options(AutoContrast PRIVATE -fPIC -fvisibility=hidden -fvisibility-inlines-hidden)

# Order matters for static archives: our code first, then libPCL, then what it uses.
target_link_libraries(AutoContrast PRIVATE ac_core ${PCL_LIBRARIES})

set_target_properties(AutoContrast PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "AutoContrast-pxm"
    LINK_FLAGS "-shared -Wl,-z,defs -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map"
)
target_compile_features(AutoContrast PRIVATE cxx_std_20)
set_property(TARGET AutoContrast APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map)
