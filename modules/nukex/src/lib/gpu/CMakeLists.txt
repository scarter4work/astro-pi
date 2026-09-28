# OpenCL is optional — if not found, library compiles with CPU-only paths
find_package(OpenCL QUIET)

# Generate embedded kernel sources from .cl files
include(${CMAKE_CURRENT_SOURCE_DIR}/generate_embedded_kernels.cmake)

set(GPU_SOURCES
    src/gpu_context.cpp
    src/gpu_shadow_buffers.cpp
    src/gpu_cpu_fallback.cpp
    src/gpu_kernels.cpp
    src/gpu_executor.cpp
    src/fit_heartbeat.cpp
)

add_library(nukex4_gpu STATIC ${GPU_SOURCES})

target_include_directories(nukex4_gpu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}  # For generated embedded_kernels.hpp
        ${CMAKE_SOURCE_DIR}/src/lib/stacker/include  # FrameCache header (no link dep)
        ${CMAKE_SOURCE_DIR}/src/lib/io/include        # Image header (no link dep)
)

# Note: nukex4_stacker is NOT linked here to avoid circular dependency.
# The FrameCache symbols are resolved at final executable link time.
# nukex4_io is needed for Image (used in executor public API).
target_link_libraries(nukex4_gpu
    PUBLIC nukex4_core nukex4_classify nukex4_io
)

if(OpenCL_FOUND)
    target_link_libraries(nukex4_gpu PRIVATE OpenCL::OpenCL)
    target_compile_definitions(nukex4_gpu PUBLIC NUKEX_HAS_OPENCL=1)
    target_compile_definitions(nukex4_gpu PRIVATE CL_TARGET_OPENCL_VERSION=300)
    message(STATUS "OpenCL found -- GPU acceleration enabled")
else()
    target_compile_definitions(nukex4_gpu PUBLIC NUKEX_HAS_OPENCL=0)
    message(STATUS "OpenCL not found -- building with CPU fallback only")
endif()

target_compile_features(nukex4_gpu PUBLIC cxx_std_17)

# The per-voxel Ceres fitting loop in execute_phase_b() is parallelised with
# nukex::parallel_for_dynamic (std::thread), NOT OpenMP: OpenMP made the
# module NEED libgomp.so.1, which stock Debian/Ubuntu do not ship, so NukeX
# failed to load there. Threads is all it needs.
find_package(Threads REQUIRED)
target_link_libraries(nukex4_gpu PUBLIC Threads::Threads)
