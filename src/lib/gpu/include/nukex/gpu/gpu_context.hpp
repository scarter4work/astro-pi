#pragma once

#include "nukex/gpu/gpu_config.hpp"
#include <vector>
#include <string>

// Forward-declare OpenCL types to avoid including cl.h in the header.
// Consumers that need the actual cl_* handles include <CL/cl.h> themselves.
#if NUKEX_HAS_OPENCL
typedef struct _cl_context*       cl_context;
typedef struct _cl_command_queue*  cl_command_queue;
typedef struct _cl_device_id*     cl_device_id;
typedef struct _cl_program*       cl_program;
#endif

namespace nukex {

/// Manages OpenCL context, device, and command queue.
/// Falls back to CPU_FALLBACK if no OpenCL runtime or no GPU available.
class GPUContext {
public:
    /// Create a context, selecting the best available GPU.
    /// If OpenCL is unavailable or force_cpu_fallback is set, backend is CPU_FALLBACK.
    static GPUContext create(const GPUExecutorConfig& config = {});

    /// List all available OpenCL devices.
    static std::vector<GPUDeviceInfo> enumerate_devices();

    bool is_gpu_available() const { return backend_ == GPUBackend::OPENCL; }
    GPUBackend backend() const { return backend_; }
    const GPUDeviceInfo& device_info() const { return device_info_; }

    /// Estimate how many voxels fit in a single VRAM batch.
    int estimate_batch_size(int n_frames, int n_channels) const;

#if NUKEX_HAS_OPENCL
    cl_context       context() const { return context_; }
    cl_command_queue  queue() const { return queue_; }
    cl_device_id     device() const { return device_; }
#endif

    ~GPUContext();
    GPUContext(GPUContext&& other) noexcept;
    GPUContext& operator=(GPUContext&& other) noexcept;

    // No copy
    GPUContext(const GPUContext&) = delete;
    GPUContext& operator=(const GPUContext&) = delete;

private:
    GPUContext() = default;

    GPUBackend  backend_     = GPUBackend::CPU_FALLBACK;
    GPUDeviceInfo device_info_ = {};

#if NUKEX_HAS_OPENCL
    cl_context       context_ = nullptr;
    cl_command_queue  queue_   = nullptr;
    cl_device_id     device_  = nullptr;
#endif
};

} // namespace nukex
