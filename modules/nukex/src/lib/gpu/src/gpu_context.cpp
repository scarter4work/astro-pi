#include "nukex/gpu/gpu_context.hpp"

#if NUKEX_HAS_OPENCL
#include <CL/cl.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <climits>
#include <fstream>
#include <string>

namespace nukex {

#if NUKEX_HAS_OPENCL

static GPUDeviceInfo query_device_info(cl_device_id dev) {
    GPUDeviceInfo info;
    char buf[256] = {};

    clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(buf), buf, nullptr);
    info.name = buf;
    std::memset(buf, 0, sizeof(buf));

    clGetDeviceInfo(dev, CL_DEVICE_VENDOR, sizeof(buf), buf, nullptr);
    info.vendor = buf;

    clGetDeviceInfo(dev, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(info.global_mem_bytes),
                    &info.global_mem_bytes, nullptr);
    clGetDeviceInfo(dev, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(info.max_compute_units),
                    &info.max_compute_units, nullptr);

    size_t wg = 0;
    clGetDeviceInfo(dev, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(wg), &wg, nullptr);
    info.max_work_group_size = static_cast<uint32_t>(wg);

    // Check for double precision support
    char exts[4096] = {};
    clGetDeviceInfo(dev, CL_DEVICE_EXTENSIONS, sizeof(exts), exts, nullptr);
    info.supports_double = (std::strstr(exts, "cl_khr_fp64") != nullptr);

    return info;
}

std::vector<GPUDeviceInfo> GPUContext::enumerate_devices() {
    std::vector<GPUDeviceInfo> result;

    cl_uint n_platforms = 0;
    clGetPlatformIDs(0, nullptr, &n_platforms);
    if (n_platforms == 0) return result;

    std::vector<cl_platform_id> platforms(n_platforms);
    clGetPlatformIDs(n_platforms, platforms.data(), nullptr);

    for (auto plat : platforms) {
        cl_uint n_devs = 0;
        clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 0, nullptr, &n_devs);
        if (n_devs == 0) continue;

        std::vector<cl_device_id> devs(n_devs);
        clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, n_devs, devs.data(), nullptr);

        for (auto dev : devs) {
            result.push_back(query_device_info(dev));
        }
    }
    return result;
}

GPUContext GPUContext::create(const GPUExecutorConfig& config) {
    GPUContext ctx;

    if (config.force_cpu_fallback) {
        ctx.backend_ = GPUBackend::CPU_FALLBACK;
        return ctx;
    }

    cl_uint n_platforms = 0;
    clGetPlatformIDs(0, nullptr, &n_platforms);
    if (n_platforms == 0) {
        ctx.backend_ = GPUBackend::CPU_FALLBACK;
        return ctx;
    }

    std::vector<cl_platform_id> platforms(n_platforms);
    clGetPlatformIDs(n_platforms, platforms.data(), nullptr);

    // Collect all GPU devices across all platforms
    struct DevEntry { cl_platform_id plat; cl_device_id dev; GPUDeviceInfo info; };
    std::vector<DevEntry> all_gpus;

    for (auto plat : platforms) {
        cl_uint n_devs = 0;
        clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 0, nullptr, &n_devs);
        if (n_devs == 0) continue;

        std::vector<cl_device_id> devs(n_devs);
        clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, n_devs, devs.data(), nullptr);

        for (auto dev : devs) {
            all_gpus.push_back({plat, dev, query_device_info(dev)});
        }
    }

    if (all_gpus.empty()) {
        ctx.backend_ = GPUBackend::CPU_FALLBACK;
        return ctx;
    }

    // Select device: user preference, or largest VRAM
    int idx = 0;
    if (config.preferred_device >= 0 &&
        config.preferred_device < static_cast<int>(all_gpus.size())) {
        idx = config.preferred_device;
    } else {
        // Pick device with most VRAM
        for (int i = 1; i < static_cast<int>(all_gpus.size()); i++) {
            if (all_gpus[i].info.global_mem_bytes > all_gpus[idx].info.global_mem_bytes)
                idx = i;
        }
    }

    auto& chosen = all_gpus[idx];

    // Create context and command queue
    cl_int err = 0;
    cl_context_properties props[] = {
        CL_CONTEXT_PLATFORM, reinterpret_cast<cl_context_properties>(chosen.plat),
        0
    };
    ctx.context_ = clCreateContext(props, 1, &chosen.dev, nullptr, nullptr, &err);
    if (err != CL_SUCCESS || !ctx.context_) {
        ctx.backend_ = GPUBackend::CPU_FALLBACK;
        return ctx;
    }

    // Use clCreateCommandQueueWithProperties for OpenCL 2.0+
    cl_queue_properties qprops[] = { 0 };
    ctx.queue_ = clCreateCommandQueueWithProperties(ctx.context_, chosen.dev, qprops, &err);
    if (err != CL_SUCCESS || !ctx.queue_) {
        clReleaseContext(ctx.context_);
        ctx.context_ = nullptr;
        ctx.backend_ = GPUBackend::CPU_FALLBACK;
        return ctx;
    }

    ctx.device_ = chosen.dev;
    ctx.device_info_ = chosen.info;
    ctx.backend_ = GPUBackend::OPENCL;
    return ctx;
}

GPUContext::~GPUContext() {
    if (queue_) clReleaseCommandQueue(queue_);
    if (context_) clReleaseContext(context_);
}

GPUContext::GPUContext(GPUContext&& other) noexcept
    : backend_(other.backend_), device_info_(std::move(other.device_info_)),
      context_(other.context_), queue_(other.queue_), device_(other.device_) {
    other.context_ = nullptr;
    other.queue_ = nullptr;
    other.device_ = nullptr;
    other.backend_ = GPUBackend::CPU_FALLBACK;
}

GPUContext& GPUContext::operator=(GPUContext&& other) noexcept {
    if (this != &other) {
        if (queue_) clReleaseCommandQueue(queue_);
        if (context_) clReleaseContext(context_);
        backend_ = other.backend_;
        device_info_ = std::move(other.device_info_);
        context_ = other.context_;
        queue_ = other.queue_;
        device_ = other.device_;
        other.context_ = nullptr;
        other.queue_ = nullptr;
        other.device_ = nullptr;
        other.backend_ = GPUBackend::CPU_FALLBACK;
    }
    return *this;
}

#else // !NUKEX_HAS_OPENCL

std::vector<GPUDeviceInfo> GPUContext::enumerate_devices() {
    return {};
}

GPUContext GPUContext::create(const GPUExecutorConfig&) {
    GPUContext ctx;
    ctx.backend_ = GPUBackend::CPU_FALLBACK;
    return ctx;
}

GPUContext::~GPUContext() {}
GPUContext::GPUContext(GPUContext&& other) noexcept
    : backend_(other.backend_), device_info_(std::move(other.device_info_)) {
    other.backend_ = GPUBackend::CPU_FALLBACK;
}
GPUContext& GPUContext::operator=(GPUContext&& other) noexcept {
    backend_ = other.backend_;
    device_info_ = std::move(other.device_info_);
    other.backend_ = GPUBackend::CPU_FALLBACK;
    return *this;
}

#endif // NUKEX_HAS_OPENCL

namespace {

// Currently-available system RAM in bytes (Linux). MemAvailable already
// accounts for reclaimable page cache, so it is the honest "how much can we
// allocate right now" figure. Returns 0 if it can't be read, letting the
// caller fall back to a conservative constant.
size_t available_host_bytes() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        // Format: "MemAvailable:   12345678 kB"
        if (line.rfind("MemAvailable:", 0) == 0) {
            unsigned long long kb = 0;
            if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kb) == 1)
                return static_cast<size_t>(kb) * 1024;
        }
    }
    return 0;
}

} // namespace

int GPUContext::batch_size_for_budgets(int n_frames, int n_channels,
                                       size_t gpu_budget_bytes,
                                       size_t host_budget_bytes) {
    // Memory per voxel in the shadow buffers (mirrored host + device):
    // welford: 3 floats * n_channels
    // pixel_values: n_frames * n_channels
    // pixel_weights: n_frames * n_channels (intermediate)
    // classification output: 6 floats + 2 shorts
    // robust + distribution + selection output: 9 * n_channels floats
    // Bookkeeping: 32 bytes padding
    size_t per_voxel =
        sizeof(float) * n_channels * 3                    // welford
      + sizeof(float) * n_channels * n_frames             // pixel_values
      + sizeof(float) * n_channels * n_frames             // pixel_weights
      + sizeof(float) * 6 + sizeof(uint16_t) * 2          // classification
      + sizeof(float) * n_channels * 9                    // robust + dist + output
      + 32;                                               // padding
    if (per_voxel == 0) per_voxel = 1;

    // The batch must fit BOTH in device memory and in host RAM, so bound by
    // the smaller of the two budgets.
    size_t budget = std::min(gpu_budget_bytes, host_budget_bytes);
    size_t batch = budget / per_voxel;
    if (batch < 1) batch = 1;
    if (batch > static_cast<size_t>(INT_MAX)) batch = static_cast<size_t>(INT_MAX);
    return static_cast<int>(batch);
}

int GPUContext::estimate_batch_size(int n_frames, int n_channels) const {
    // Device (VRAM) budget — only constrains when running on the GPU.
    size_t gpu_budget = SIZE_MAX;
    if (backend_ == GPUBackend::OPENCL && device_info_.global_mem_bytes > 0)
        gpu_budget = static_cast<size_t>(device_info_.global_mem_bytes) * 85 / 100;

    // Host (system RAM) budget — the shadow buffers live in host RAM too, and
    // PixInsight is already holding the loaded frames + the output cube. Keep
    // only half of what is free right now as headroom for that growth and the
    // GPU host-staging copies, so Phase B can never OOM-kill the process (the
    // failure that a VRAM-only estimate caused on a 16 GB GPU / 30 GB host).
    size_t host_avail = available_host_bytes();
    size_t host_budget = host_avail ? host_avail / 2
                                    : 2ULL * 1024 * 1024 * 1024;  // fallback working set

    return batch_size_for_budgets(n_frames, n_channels, gpu_budget, host_budget);
}

} // namespace nukex
