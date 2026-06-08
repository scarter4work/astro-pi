#pragma once

#include "nukex/gpu/gpu_context.hpp"
#include <string>

#if NUKEX_HAS_OPENCL
typedef struct _cl_kernel* cl_kernel;
#endif

namespace nukex {

/// Manages OpenCL kernel compilation and caching.
///
/// Kernel sources are embedded as string literals at compile time
/// (no external .cl files needed at runtime). The common.cl header
/// is prepended to each kernel source before compilation.
class GPUKernels {
public:
    /// Compile all kernels for the given context.
    /// Returns false if compilation fails (falls back to CPU).
    bool compile(const GPUContext& ctx);

    /// Release all compiled kernels.
    void release();

    ~GPUKernels() { release(); }

    bool is_compiled() const { return compiled_; }

#if NUKEX_HAS_OPENCL
    cl_kernel classify_weights_kernel() const { return classify_weights_; }
    cl_kernel robust_stats_kernel() const { return robust_stats_; }
    cl_kernel select_pixels_kernel() const { return select_pixels_; }
    cl_kernel spatial_context_kernel() const { return spatial_context_; }
#endif

private:
    bool compiled_ = false;

#if NUKEX_HAS_OPENCL
    cl_kernel classify_weights_ = nullptr;
    cl_kernel robust_stats_     = nullptr;
    cl_kernel select_pixels_    = nullptr;
    cl_kernel spatial_context_  = nullptr;

    // Build a single kernel from source
    cl_kernel build_kernel(const GPUContext& ctx,
                           const std::string& source,
                           const char* kernel_name);
#endif
};

} // namespace nukex
