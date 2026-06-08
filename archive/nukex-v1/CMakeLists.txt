# NukeX - Intelligent Region-Aware Stretch for PixInsight
# Copyright (c) 2026 Scott Carter
# CMake build configuration

cmake_minimum_required(VERSION 3.15)
project(NukeX VERSION 2.0.0 LANGUAGES CXX)

# C++17 required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Options
option(NUKEX_USE_ONNX "Enable ONNX Runtime support for neural segmentation" OFF)

# PixInsight SDK
if(NOT DEFINED PCLDIR)
    if(DEFINED ENV{PCLDIR})
        set(PCLDIR $ENV{PCLDIR})
    else()
        message(FATAL_ERROR "PCLDIR not set. Please set PCLDIR environment variable or pass -DPCLDIR=...")
    endif()
endif()

# PCL include/lib paths
set(PCL_INCLUDE_DIR "${PCLDIR}/include")

# Platform-specific settings
if(WIN32)
    set(PCL_LIB_DIR "${PCLDIR}/lib/x64")
    set(PCL_LIBRARIES
        PCL-pxi lz4-pxi zstd-pxi zlib-pxi RFC6234-pxi lcms-pxi cminpack-pxi
    )
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    set(PCL_LIB_DIR "${PCLDIR}/lib/x64")
    set(PCL_LIBRARIES
        PCL-pxi lz4-pxi zstd-pxi zlib-pxi RFC6234-pxi lcms-pxi cminpack-pxi
    )
else()
    # Linux uses static libraries in lib/x64
    set(PCL_LIB_DIR "${PCLDIR}/lib/x64")
    set(PCL_LIBRARIES
        PCL-pxi lz4-pxi zstd-pxi zlib-pxi RFC6234-pxi lcms-pxi cminpack-pxi
    )
endif()

# OpenMP support for parallel processing
find_package(OpenMP)

# Source files
set(NUKEX_SOURCES
    src/NukeXModule.cpp
    src/NukeXProcess.cpp
    src/NukeXInstance.cpp
    src/NukeXInterface.cpp
    src/NukeXParameters.cpp
    # Stack integration process
    src/NukeXStackProcess.cpp
    src/NukeXStackInstance.cpp
    src/NukeXStackParameters.cpp
    src/NukeXStackInterface.cpp
    # Engine core
    src/engine/IStretchAlgorithm.cpp
    src/engine/StretchLibrary.cpp
    src/engine/RegionStatistics.cpp
    src/engine/HistogramEngine.cpp
    src/engine/RegionAnalyzer.cpp
    src/engine/SelectionRules.cpp
    src/engine/StretchSelector.cpp
    src/engine/BlendEngine.cpp
    src/engine/ToneMapper.cpp
    src/engine/LRGBProcessor.cpp
    src/engine/Compositor.cpp
    # Layer-aware components
    src/engine/PSFModel.cpp
    # Distribution fitting for selective stacking
    src/engine/DistributionFitter.cpp
    # Frame processing utilities
    src/engine/BayerDemosaic.cpp
    src/engine/FrameStreamer.cpp
    src/engine/FrameRegistration.cpp
    # Pixel stack analysis and selection
    src/engine/PixelStackAnalyzer.cpp
    src/engine/PixelSelector.cpp
    # Transition detection and smoothing
    src/engine/TransitionChecker.cpp
    # Algorithm implementations
    src/engine/algorithms/MTFStretch.cpp
    src/engine/algorithms/GHStretch.cpp
    src/engine/algorithms/ArcSinhStretch.cpp
    src/engine/algorithms/HistogramStretch.cpp
    src/engine/algorithms/LogStretch.cpp
    src/engine/algorithms/LumptonStretch.cpp
    src/engine/algorithms/RNCStretch.cpp
    src/engine/algorithms/PhotometricStretch.cpp
    src/engine/algorithms/OTSStretch.cpp
    src/engine/algorithms/SASStretch.cpp
    src/engine/algorithms/VeraluxStretch.cpp
    src/engine/algorithms/StatisticalAutoStretch.cpp
)

set(NUKEX_HEADERS
    src/NukeXModule.h
    src/NukeXProcess.h
    src/NukeXInstance.h
    src/NukeXInterface.h
    src/NukeXParameters.h
    # Stack integration process
    src/NukeXStackProcess.h
    src/NukeXStackInstance.h
    src/NukeXStackParameters.h
    src/NukeXStackInterface.h
    # Engine core
    src/engine/IStretchAlgorithm.h
    src/engine/StretchLibrary.h
    src/engine/RegionStatistics.h
    src/engine/HistogramEngine.h
    src/engine/RegionAnalyzer.h
    src/engine/SelectionRules.h
    src/engine/StretchSelector.h
    src/engine/BlendEngine.h
    src/engine/ToneMapper.h
    src/engine/LRGBProcessor.h
    src/engine/Compositor.h
    src/engine/Constants.h
    # Layer-aware components
    src/engine/PSFModel.h
    # Distribution fitting for selective stacking
    src/engine/DistributionFitter.h
    # Frame processing utilities
    src/engine/BayerDemosaic.h
    src/engine/FrameStreamer.h
    src/engine/FrameRegistration.h
    # Pixel stack analysis and selection
    src/engine/PixelStackAnalyzer.h
    src/engine/PixelSelector.h
    # Transition detection and smoothing
    src/engine/TransitionChecker.h
    # Algorithm headers
    src/engine/algorithms/MTFStretch.h
    src/engine/algorithms/GHStretch.h
    src/engine/algorithms/ArcSinhStretch.h
    src/engine/algorithms/HistogramStretch.h
    src/engine/algorithms/LogStretch.h
    src/engine/algorithms/LumptonStretch.h
    src/engine/algorithms/RNCStretch.h
    src/engine/algorithms/PhotometricStretch.h
    src/engine/algorithms/OTSStretch.h
    src/engine/algorithms/SASStretch.h
    src/engine/algorithms/VeraluxStretch.h
    src/engine/algorithms/StatisticalAutoStretch.h
)

# Create shared library
add_library(NukeX SHARED ${NUKEX_SOURCES} ${NUKEX_HEADERS})

# Include directories
target_include_directories(NukeX PRIVATE
    ${PCL_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine
    ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/algorithms
)

# Link directories and libraries
target_link_directories(NukeX PRIVATE ${PCL_LIB_DIR})
target_link_libraries(NukeX PRIVATE ${PCL_LIBRARIES} pthread)

# OpenMP support
if(OpenMP_CXX_FOUND)
    target_compile_options(NukeX PRIVATE ${OpenMP_CXX_FLAGS})
    target_link_libraries(NukeX PRIVATE OpenMP::OpenMP_CXX)
endif()

# ONNX Runtime support (ML segmentation)
if(NUKEX_USE_ONNX)
    # Add ONNX/ML source files
    target_sources(NukeX PRIVATE
        src/engine/ONNXInference.cpp
        src/engine/SegmentationModel.cpp
        src/engine/Segmentation.cpp
    )

    # Find ONNX Runtime
    find_package(onnxruntime QUIET)
    if(onnxruntime_FOUND)
        target_link_libraries(NukeX PRIVATE onnxruntime::onnxruntime)
    else()
        # Try system paths
        target_include_directories(NukeX PRIVATE /usr/include/onnxruntime)
        target_link_libraries(NukeX PRIVATE onnxruntime)
    endif()
    target_compile_definitions(NukeX PRIVATE NUKEX_USE_ONNX ONNX_AVAILABLE)
endif()

# Compiler flags (matching PCL build system)
if(MSVC)
    target_compile_options(NukeX PRIVATE /W4 /O2)
else()
    target_compile_options(NukeX PRIVATE
        -Wall -Wno-parentheses
        -O3 -fPIC
        -ffunction-sections -fdata-sections
        -fvisibility=hidden -fvisibility-inlines-hidden
        -fnon-call-exceptions
    )
    target_compile_definitions(NukeX PRIVATE _REENTRANT __PCL_BUILDING_MODULE)
    if(NOT APPLE)
        target_link_options(NukeX PRIVATE
            -Wl,-z,noexecstack
            -Wl,-O1
            -Wl,--gc-sections
            -rdynamic
        )
    endif()
endif()

# Platform definitions
if(WIN32)
    target_compile_definitions(NukeX PRIVATE __PCL_WINDOWS)
elseif(APPLE)
    target_compile_definitions(NukeX PRIVATE __PCL_MACOSX)
else()
    target_compile_definitions(NukeX PRIVATE __PCL_LINUX)
endif()

# Output configuration - PixInsight module naming convention
set_target_properties(NukeX PROPERTIES
    OUTPUT_NAME "NukeX-pxm"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Install
install(TARGETS NukeX
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# Copy icon
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icons/NukeX.svg")
    install(FILES icons/NukeX.svg DESTINATION share/NukeX/icons)
endif()

# Print configuration
message(STATUS "")
message(STATUS "NukeX Configuration:")
message(STATUS "  Version:       ${PROJECT_VERSION}")
message(STATUS "  PCLDIR:        ${PCLDIR}")
message(STATUS "  ONNX Support:  ${NUKEX_USE_ONNX}")
if(NUKEX_USE_ONNX)
    message(STATUS "  ONNX Runtime:  ${ONNXRUNTIME_DIR}")
endif()
message(STATUS "")
