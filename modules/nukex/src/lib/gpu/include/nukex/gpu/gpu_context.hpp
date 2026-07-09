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

    /// Estimate how many voxels fit in a single Phase-B batch, bounded by the
    /// SMALLER of GPU VRAM and available system RAM. The shadow buffers are
    /// allocated in host RAM as well as on the device, so host memory is often
    /// the tighter constraint (e.g. a large-VRAM GPU on a modest-RAM host); a
    /// VRAM-only estimate can drive the process into swap / OOM.
    int estimate_batch_size(int n_frames, int n_channels) const;

    /// Pure batch-size calculation from explicit memory budgets (bytes).
    /// Exposed so the sizing logic can be unit-tested without a GPU or a
    /// particular host-memory state. Returns voxels that fit in
    /// min(gpu_budget, host_budget), clamped to [1, INT_MAX].
    static int batch_size_for_budgets(int n_frames, int n_channels,
                                      size_t gpu_budget_bytes,
                                      size_t host_budget_bytes);

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
