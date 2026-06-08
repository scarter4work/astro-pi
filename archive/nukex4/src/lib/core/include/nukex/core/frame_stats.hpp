#pragma once

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
};

} // namespace nukex
