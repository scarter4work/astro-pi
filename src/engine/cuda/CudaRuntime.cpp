// src/engine/cuda/CudaRuntime.cpp
#include "cuda/CudaRuntime.h"
#include <cstring>

namespace nukex {
namespace cuda {

#ifdef NUKEX_HAS_CUDA

static bool s_probed = false;
static bool s_available = false;
static char s_gpuName[256] = {};
static size_t s_memoryMB = 0;

static void probeOnce() {
    if (s_probed) return;
    s_probed = true;

    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess || deviceCount == 0) return;

    cudaDeviceProp prop;
    err = cudaGetDeviceProperties(&prop, 0);
    if (err != cudaSuccess) return;

    s_available = true;
    std::strncpy(s_gpuName, prop.name, sizeof(s_gpuName) - 1);
    s_gpuName[sizeof(s_gpuName) - 1] = '\0';
    s_memoryMB = prop.totalGlobalMem / (1024 * 1024);
}

bool isGpuAvailable() { probeOnce(); return s_available; }
const char* gpuName() { probeOnce(); return s_gpuName; }
size_t gpuMemoryMB() { probeOnce(); return s_memoryMB; }

#else

bool isGpuAvailable() { return false; }
const char* gpuName() { return ""; }
size_t gpuMemoryMB() { return 0; }

#endif

} // namespace cuda
} // namespace nukex
