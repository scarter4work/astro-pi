# PCLConfig.cmake -- PixInsight Class Library 2.13 for AutoContrast.
# Headers AND static libraries come from PCLDIR (no fallback to /opt/PixInsight/include: those
# headers next to a different libPCL would put two layouts of the same classes in one binary).
# Shipped builds use the image's /opt/pcl (tools/build-env); a host build uses ~/PCL (2.13).
set(PCLDIR "$ENV{PCLDIR}" CACHE PATH "Path to PCL SDK")
if(NOT PCLDIR)
    set(PCLDIR "$ENV{HOME}/PCL")
endif()

set(PCL_INCLUDE_DIR "${PCLDIR}/include")
set(PCL_LIB_DIR "${PCLDIR}/lib/x64")

if(NOT EXISTS "${PCL_INCLUDE_DIR}/pcl/MetaModule.h")
    message(FATAL_ERROR "PCL not found at ${PCLDIR} (no include/pcl/MetaModule.h). Set PCLDIR.")
endif()
# 2.13 marker: the TreeBox additions that make a 2.13 module need PixInsight >= 1.9.6.
if(NOT EXISTS "${PCL_INCLUDE_DIR}/pcl/BlindAstrometricSolver.h")
    message(FATAL_ERROR "PCL at ${PCLDIR} is older than 2.13 (no BlindAstrometricSolver.h); AutoContrast builds against PCL 2.13 only.")
endif()

set(PCL_LIB_NAMES PCL-pxi lz4-pxi zstd-pxi zlib-pxi RFC6234-pxi lcms-pxi cminpack-pxi)
set(PCL_LIBRARIES "")
foreach(lib ${PCL_LIB_NAMES})
    find_library(PCL_${lib}_LIBRARY NAMES ${lib} PATHS "${PCL_LIB_DIR}" NO_DEFAULT_PATH)
    if(NOT PCL_${lib}_LIBRARY)
        message(FATAL_ERROR "PCL library ${lib} not found in ${PCL_LIB_DIR}")
    endif()
    list(APPEND PCL_LIBRARIES "${PCL_${lib}_LIBRARY}")
endforeach()

set(PCL_FOUND TRUE)
set(PCL_INCLUDE_DIRS "${PCL_INCLUDE_DIR}")
message(STATUS "PCL 2.13 SDK: ${PCLDIR}")
