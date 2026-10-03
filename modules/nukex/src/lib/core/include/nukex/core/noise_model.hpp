#pragma once

#include "nukex/core/frame_stats.hpp"

namespace nukex {

/// The per-sample noise model behind Phase B's pixel selection (kernel 3).
///
/// One CPU source of truth. GPUCPUFallback::select_pixels calls this, and
/// select_pixels.cl carries the same arithmetic for the GPU;
/// test_gpu_agreement holds the two together. There is no third copy: an
/// untested duplicate is how the CPU and GPU paths drifted apart before.
struct NoiseModel {
    /// Variance of one sample, in normalized^2 units.
    ///
    /// `slot` is the cube slot the sample belongs to -- the same index the
    /// GPU kernels call `ch`. It selects the normalisation this frame had
    /// applied in Phase A, which must be undone before the Poisson term,
    /// since that term is only meaningful on raw ADU.
    ///
    /// `fallback_var` is used when the frame carries no usable GAIN/RDNOISE
    /// keywords: kernel 2's robust across-frame scale, squared, or Welford
    /// where that scale is degenerate. The caller decides which; this
    /// function only says when it applies.
    static float sample_variance(float value, const FrameStats& fs, int slot,
                                 float fallback_var);
};

} // namespace nukex
