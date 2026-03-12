// src/engine/cuda/CudaRuntime.h
#pragma once

#ifdef NUKEX_HAS_CUDA
#include <cuda_runtime.h>
#endif

#include <cstddef>

namespace nukex {
namespace cuda {

// Returns true if a CUDA-capable GPU is available at runtime
bool isGpuAvailable();

// Returns GPU name string (empty if unavailable)
const char* gpuName();

// Returns GPU memory in MB (0 if unavailable)
size_t gpuMemoryMB();

} // namespace cuda
} // namespace nukex
