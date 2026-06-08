#pragma once

#include "nukex/gpu/gpu_config.hpp"
#include "nukex/gpu/gpu_context.hpp"
#include "nukex/gpu/gpu_kernels.hpp"
#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/gpu/gpu_cpu_fallback.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/io/image.hpp"
#include <vector>
#include <functional>

namespace nukex {

class FrameCache;
class ModelSelector;

/// Orchestrates GPU-accelerated Phase B processing.
///
/// Batch loop per VRAM batch:
///   1. Extract voxel data into SoA shadow buffers
///   2. GPU kernel 1: classify_weights
///   3. GPU kernel 2: robust_stats
///   4. Download + writeback classification/stats to voxels
///   5. CPU: distribution fitting (Ceres — cannot run on GPU)
///   6. Upload fitted distributions
///   7. GPU kernel 3: select_pixels
///   8. Download + writeback output values
///
/// After all batches:
///   9. GPU kernel 4: spatial_context on stacked output
///
/// CPU fallback: identical code path using GPUCPUFallback instead of
/// OpenCL dispatch. Selected automatically when no GPU or force_cpu_fallback.
class GPUExecutor {
public:
    explicit GPUExecutor(const GPUExecutorConfig& config = {});

    /// Run the full Phase B pipeline on the cube.
    /// fitting_fn: called per-voxel to run distribution fitting (Ceres).
    ///   signature: void(SubcubeVoxel& voxel, const float* values,
    ///                    const float* weights, int n_frames, int n_channels,
    ///                    const FrameStats* frame_stats)
    using FittingFn = std::function<void(SubcubeVoxel&, const float*, const float*,
                                          int, int, const FrameStats*)>;

    void execute_phase_b(
        Cube& cube,
        FrameCache& cache,
        const std::vector<FrameStats>& frame_stats,
        const WeightConfig& weight_config,
        FittingFn fitting_fn,
        Image& stacked_output,
        Image& noise_output,
        ProgressObserver* progress = nullptr);

    /// Run spatial context on the stacked output.
    void execute_spatial_context(
        const Image& stacked,
        Cube& cube,
        ProgressObserver* progress = nullptr);

    GPUBackend active_backend() const { return context_.backend(); }
    const GPUDeviceInfo& device_info() const { return context_.device_info(); }

    /// Execute kernels 1+2 on a shadow buffer batch.
    /// Public for GPU vs CPU agreement testing.
    void execute_batch_gpu(ShadowBuffers& buf, const FrameStats* fs,
                           const WeightConfig& wc, int batch_size,
                           int n_channels, int n_frames);

    void execute_batch_cpu(ShadowBuffers& buf, const FrameStats* fs,
                           const WeightConfig& wc, int batch_size,
                           int n_channels, int n_frames);

    /// Execute kernel 3 (select_pixels) on GPU for a batch.
    void execute_select_gpu(ShadowBuffers& buf, const FrameStats* fs,
                            int batch_size, int n_channels, int n_frames);

    /// Execute kernel 4 (spatial_context) on GPU.
    void execute_spatial_gpu(const Image& stacked,
                              float* gradient_mag, float* local_background,
                              float* local_rms);

private:
    GPUContext   context_;
    GPUKernels   kernels_;
};

} // namespace nukex
