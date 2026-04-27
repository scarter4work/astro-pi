# PCLConfig.cmake — Find PixInsight Class Library SDK
set(PCLDIR "$ENV{PCLDIR}" CACHE PATH "Path to PCL SDK")
if(NOT PCLDIR)
    set(PCLDIR "$ENV{HOME}/PCL")
endif()

set(PIXINSIGHT_DIR "/opt/PixInsight" CACHE PATH "Path to PixInsight installation")

find_path(PCL_INCLUDE_DIR
    NAMES pcl/MetaModule.h
    PATHS
        "${PCLDIR}/include"
        "${PIXINSIGHT_DIR}/include"
    NO_DEFAULT_PATH
)

set(PCL_LIB_DIR "${PCLDIR}/lib/x64")

set(PCL_LIB_NAMES
    PCL-pxi
    lz4-pxi
    zstd-pxi
    zlib-pxi
    RFC6234-pxi
    lcms-pxi
    cminpack-pxi
)

set(PCL_LIBRARIES "")
foreach(lib ${PCL_LIB_NAMES})
    find_library(PCL_${lib}_LIBRARY
        NAMES ${lib}
        PATHS "${PCL_LIB_DIR}"
        NO_DEFAULT_PATH
    )
    if(PCL_${lib}_LIBRARY)
        list(APPEND PCL_LIBRARIES "${PCL_${lib}_LIBRARY}")
    else()
        message(WARNING "PCL library not found: ${lib}")
    endif()
endforeach()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(PCL_MODULE_EXT ".so")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(PCL_MODULE_EXT ".dylib")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(PCL_MODULE_EXT ".dll")
endif()

if(PCL_INCLUDE_DIR AND PCL_LIBRARIES)
    set(PCL_FOUND TRUE)
    set(PCL_INCLUDE_DIRS "${PCL_INCLUDE_DIR}")
    message(STATUS "PCL SDK found: ${PCLDIR}")
    message(STATUS "PCL headers: ${PCL_INCLUDE_DIR}")
    message(STATUS "PCL libraries: ${PCL_LIBRARIES}")
else()
    set(PCL_FOUND FALSE)
    message(FATAL_ERROR "PCL SDK not found. Set PCLDIR or install PixInsight.")
endif()
