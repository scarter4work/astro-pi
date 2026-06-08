#pragma once

#include "nukex/core/types.hpp"
#include "nukex/core/welford.hpp"
#include "nukex/core/histogram.hpp"
#include "nukex/core/distribution.hpp"
#include <cstdint>

namespace nukex {

namespace VoxelFlags {
    constexpr uint8_t BORDER     = 0x01;
    constexpr uint8_t SATURATED  = 0x02;
    constexpr uint8_t LOW_N      = 0x04;
    constexpr uint8_t FIT_FAILED = 0x08;
}

struct SubcubeVoxel {
    // ── Streaming accumulators (Phase A) ─────────────────────────────
    WelfordAccumulator  welford[MAX_CHANNELS];
    PixelHistogram      histogram[MAX_CHANNELS];

    // ── Fitted distribution (Phase B, per-channel) ───────────────────
    ZDistribution       distribution[MAX_CHANNELS];

    // ── Per-channel output (Phase B) ─────────────────────────────────
    float               snr[MAX_CHANNELS]                    = {};

    // ── Per-channel robust statistics (Phase B) ──────────────────────
    float               mad[MAX_CHANNELS]                    = {};
    float               biweight_midvariance[MAX_CHANNELS]   = {};
    float               iqr[MAX_CHANNELS]                    = {};

    // ── Classification summaries (Phase B) ───────────────────────────
    uint16_t            cloud_frame_count  = 0;
    uint16_t            trail_frame_count  = 0;
    float               worst_sigma_score  = 0.0f;
    float               best_sigma_score   = 0.0f;
    float               mean_weight        = 0.0f;
    float               total_exposure     = 0.0f;

    // ── Cross-channel quality (Phase B) ──────────────────────────────
    float               confidence         = 0.0f;
    float               quality_score      = 0.0f;
    DistributionShape   dominant_shape     = DistributionShape::UNKNOWN;

    // ── Spatial context (Phase B, post-selection) ────────────────────
    float               gradient_mag       = 0.0f;
    float               local_background   = 0.0f;
    float               local_rms          = 0.0f;

    // ── PSF quality at this position ─────────────────────────────────
    float               mean_fwhm          = 0.0f;
    float               mean_eccentricity  = 0.0f;
    float               best_fwhm          = 0.0f;

    // ── Bookkeeping ──────────────────────────────────────────────────
    uint16_t            n_frames           = 0;
    uint8_t             n_channels         = 0;
    uint8_t             flags              = 0;

    bool has_flag(uint8_t flag) const { return (flags & flag) != 0; }
    void set_flag(uint8_t flag)       { flags |= flag; }
    void clear_flag(uint8_t flag)     { flags &= ~flag; }
};

} // namespace nukex
