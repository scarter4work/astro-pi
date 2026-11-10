# EZTools PixInsight module -> EZTools-pxm.so (Linux).
# PCLConfig (included by the top level) has already checked PCLDIR is PCL 2.13.

set(MODULE_SOURCES
    EZToolsModule.cpp
    EZHazeKillProcess.cpp
    EZHazeKillParameters.cpp
    EZHazeKillInstance.cpp
    EZHazeKillInterface.cpp
)

add_library(EZTools SHARED ${MODULE_SOURCES})

target_include_directories(EZTools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/..")
# PCL headers are SYSTEM includes so warnings inside PCL don't flood the build.
target_include_directories(EZTools SYSTEM PRIVATE ${PCL_INCLUDE_DIRS})

target_compile_definitions(EZTools PRIVATE __PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT)
target_compile_options(EZTools PRIVATE -fPIC -fvisibility=hidden -fvisibility-inlines-hidden)

# Order matters for static archives: the engine, libPCL, then what libPCL uses.
target_link_libraries(EZTools PRIVATE eztools_hazekill ${PCL_LIBRARIES})

set_target_properties(EZTools PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "EZTools-pxm"
    LINK_FLAGS "-shared -Wl,-z,defs -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map"
)
target_compile_features(EZTools PRIVATE cxx_std_20)
set_property(TARGET EZTools APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/pixinsight-module.map)
