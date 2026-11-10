// Near-side dust layer: a port of recon/extinction.py (PCL-free).
//
// A dark lane is dust only when it is dark relative to a bright neighbourhood; the plain sky
// floor is equally dark but is not dust. Operates on linear data and never stretches it.
#pragma once

#include "Model.h"

namespace gdg::recon {

inline constexpr double CASE_B_HA_HB = 2.86;
inline constexpr int DUST_LANE_MAX_WIDTH_PX = 15;   ///< grey closing fills narrower dark features
inline constexpr double DUST_SKY_PERCENTILE = 5.0;
inline constexpr double DUST_BRIGHT_PERCENTILE = 99.0;
inline constexpr double DUST_MIN_SURROUND_FRAC = 0.05;
inline constexpr double BALMER_SATURATION = 3.0;    ///< excess 3 (4x Case B) -> 1 - e^-1

/// dust_from_structure(lum, lane_px): a = clip(nan_to_num(lum), 0); sky / bright = its 5th / 99th
/// percentiles; smooth = gaussian_filter(a, 1); surround = grey_closing(smooth, lane_px x lane_px);
/// opacity = (surround - smooth) / max(surround - sky, 1e-12), gated by
/// clip((height - 0.05 rng) / (0.05 rng), 0, 1); clipped to [0, 1], float32. Mono only.
Image dust_from_structure(const Image& lum, int lane_px = DUST_LANE_MAX_WIDTH_PX);

/// dust_from_balmer(ha, hb, eps): excess = max(ha / (hb + eps) / 2.86 - 1, 0),
/// dust = 1 - exp(-excess / 3), times min(signal_gate(ha), signal_gate(hb)), clipped to [0, 1],
/// float32. signal_gate(a) = clip((gaussian_filter(a, 1) - sky - t) / t, 0, 1), t = 0.05 (bright -
/// sky), or 1 everywhere when bright - sky < 0.05 max(bright, 1e-12). Mono planes of one shape.
Image dust_from_balmer(const Image& ha, const Image& hb, double eps = 1e-6);

}  // namespace gdg::recon
