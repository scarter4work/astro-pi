# Generate embedded_kernels.hpp from .cl source files.
# Called during CMake configure time.

set(KERNEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/kernels")
set(OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/embedded_kernels.hpp")

set(KERNEL_FILES
    common
    classify_weights
    robust_stats
    select_pixels
    spatial_context
)

file(WRITE "${OUTPUT_FILE}" "// Auto-generated: embedded OpenCL kernel sources\n")
file(APPEND "${OUTPUT_FILE}" "// Do not edit — generated from src/lib/gpu/kernels/*.cl\n\n")
file(APPEND "${OUTPUT_FILE}" "#pragma once\n\n")
file(APPEND "${OUTPUT_FILE}" "namespace nukex { namespace gpu { namespace kernels {\n\n")

foreach(KERNEL ${KERNEL_FILES})
    set(CL_FILE "${KERNEL_DIR}/${KERNEL}.cl")
    if(EXISTS "${CL_FILE}")
        file(READ "${CL_FILE}" CL_SOURCE)
        # Escape backslashes and quotes for C++ raw string
        file(APPEND "${OUTPUT_FILE}" "inline const char* ${KERNEL}_cl = R\"OPENCL(\n")
        file(APPEND "${OUTPUT_FILE}" "${CL_SOURCE}")
        file(APPEND "${OUTPUT_FILE}" ")OPENCL\";\n\n")
    else()
        file(APPEND "${OUTPUT_FILE}" "inline const char* ${KERNEL}_cl = \"\";\n\n")
    endif()
endforeach()

file(APPEND "${OUTPUT_FILE}" "}}} // namespace nukex::gpu::kernels\n")
