// src/engine/cuda/CudaRuntime.cpp
#include "cuda/CudaRuntime.h"
#include <cstdio>
#include <cstring>
#include <mutex>

namespace nukex {
namespace cuda {

#ifdef NUKEX_HAS_CUDA

static std::once_flag s_probeFlag;
static bool s_available = false;
static char s_gpuName[256] = {};
static size_t s_memoryMB = 0;

static void probeOnce() {
    int deviceCount = 0;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err != cudaSuccess || deviceCount == 0) {
        std::fprintf(stderr, "NukeX: CUDA probe: %s\n", cudaGetErrorString(err));
        return;
    }

    cudaDeviceProp prop;
    err = cudaGetDeviceProperties(&prop, 0);
    if (err != cudaSuccess) {
        std::fprintf(stderr, "NukeX: CUDA device properties: %s\n", cudaGetErrorString(err));
        return;
    }

    s_available = true;
    std::strncpy(s_gpuName, prop.name, sizeof(s_gpuName) - 1);
    s_gpuName[sizeof(s_gpuName) - 1] = '\0';
    s_memoryMB = prop.totalGlobalMem / (1024 * 1024);
}

bool isGpuAvailable() { std::call_once(s_probeFlag, probeOnce); return s_available; }
const char* gpuName() { std::call_once(s_probeFlag, probeOnce); return s_gpuName; }
size_t gpuMemoryMB() { std::call_once(s_probeFlag, probeOnce); return s_memoryMB; }

#else

bool isGpuAvailable() { return false; }
const char* gpuName() { return ""; }
size_t gpuMemoryMB() { return 0; }

#endif

} // namespace cuda
} // namespace nukex
