#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nukex {

/// GPU device information discovered at runtime.
struct GPUDeviceInfo {
    std::string name;
    std::string vendor;
    uint64_t    global_mem_bytes     = 0;
    uint32_t    max_compute_units    = 0;
    uint32_t    max_work_group_size  = 0;
    bool        supports_double      = false;
};

/// GPU execution backend.
enum class GPUBackend { OPENCL, CPU_FALLBACK };

/// Configuration for the GPU executor.
struct GPUExecutorConfig {
    int    preferred_device     = -1;     // -1 = auto-select best GPU
    bool   force_cpu_fallback   = false;  // true = skip GPU, use CPU for testing
    size_t max_vram_usage_bytes = 0;      // 0 = auto (85% of device VRAM)
};

/// Maximum frames the GPU kernels support in a single pass.
/// Private-memory sorting in robust_stats requires a bounded array.
/// If a voxel has more frames than this, it falls back to CPU.
static constexpr int GPU_MAX_FRAMES = 512;

} // namespace nukex
