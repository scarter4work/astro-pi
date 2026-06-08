#pragma once

#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/classify/weight_computer.hpp"

namespace nukex {

/// CPU fallback implementations of all GPU kernel algorithms.
///
/// These operate on ShadowBuffers in the exact same SoA layout as the
/// GPU kernels. They produce IDENTICAL results (within IEEE 754 1-ULP
/// tolerance for transcendental functions).
///
/// The code structure mirrors the OpenCL kernels: same loop order,
/// same intermediates, same clamping. This ensures numerical agreement.
class GPUCPUFallback {
public:
    /// Kernel 1: Weight computation + classification summaries.
    /// Mirrors classify_weights.cl
    static void classify_weights(ShadowBuffers& buf,
                                  const FrameStats* frame_stats,
                                  const WeightConfig& config,
                                  int batch_size, int n_channels, int n_frames);

    /// Kernel 2: Robust statistics (MAD, biweight midvariance, IQR).
    /// Mirrors robust_stats.cl
    static void robust_stats(ShadowBuffers& buf,
                              int batch_size, int n_channels, int n_frames);

    /// Kernel 3: Pixel selection + noise propagation.
    /// Mirrors select_pixels.cl
    static void select_pixels(ShadowBuffers& buf,
                               const FrameStats* frame_stats,
                               int batch_size, int n_channels, int n_frames);

    /// Kernel 4: Spatial context (Sobel + local background/RMS).
    /// Mirrors spatial_context.cl
    /// Operates on the stacked output image, not on voxels.
    static void spatial_context(const float* stacked_data,
                                 int width, int height, int n_channels,
                                 float* gradient_mag,
                                 float* local_background,
                                 float* local_rms);
};

} // namespace nukex
