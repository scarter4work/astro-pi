#pragma once

#include "nukex/core/types.hpp"

#include <cstdint>

namespace nukex {

/// Per-frame metadata for Phase B analysis.
///
/// Populated by the stacker during Phase A (streaming). Shared read-only
/// during Phase B parallel pixel processing. Indexed by frame_index
/// (0-based position in the input light_paths vector).
struct FrameStats {
    float read_noise         = 3.0f;
    float gain               = 1.0f;
    float exposure           = 0.0f;
    bool  has_noise_keywords = false;
    bool  is_meridian_flipped = false;

    float frame_weight       = 1.0f;
    float median_luminance   = 0.0f;
    float fwhm               = 0.0f;

    float psf_weight         = 1.0f;

    /// Frame-level cloud attenuation score (1.0 = clear, <1.0 = penalized).
    /// Computed by the stacker between Phase A and Phase B using the global
    /// median of per-frame medians as reference.
    float cloud_score        = 1.0f;

    /// Per-SLOT normalisation actually applied to this frame's samples in
    /// Phase A: the cached value is `norm_scale[slot] * raw + norm_offset[slot]`.
    /// Indexed by cube slot, which is the same index the GPU calls `ch`.
    ///
    /// Phase B's noise model needs these to undo the map before it applies a
    /// Poisson term, because that term is only meaningful on raw ADU. Without
    /// them a frame scaled by `a` has its shot noise misread by the same
    /// factor -- silently, since nothing downstream re-derives it.
    ///
    /// The identity is (1, 0), and it is exact rather than approximate: the
    /// noise model's arithmetic reduces to the un-normalised expression
    /// bit-for-bit at those values, so a batch that needs no correction
    /// cannot be moved by this.
    float norm_scale[MAX_CHANNELS]  = {1.0f, 1.0f, 1.0f, 1.0f,
                                       1.0f, 1.0f, 1.0f, 1.0f};
    float norm_offset[MAX_CHANNELS] = {0.0f, 0.0f, 0.0f, 0.0f,
                                       0.0f, 0.0f, 0.0f, 0.0f};
};

static_assert(MAX_CHANNELS == 8,
              "FrameStats::norm_* initialisers are written out per slot; "
              "extend them alongside MAX_CHANNELS.");

} // namespace nukex
