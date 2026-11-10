// Nebula (starless-layer) depth grade, as the Python core's nebula.py (PCL-free, reentrant).
//
// Stars carry Gaia parallax; diffuse gas does not. The nebula depth is STRUCTURE-driven (its own
// form decides where it varies) and Gaia-CALIBRATED (the matched stars' distance spread sets how
// strong it is): physically motivated, never "measured".
#pragma once

#include "Detect.h"
#include "Image.h"

#include <optional>
#include <vector>

namespace gdg {

/// nebula.gaia_depth_budget: clip((p95 - p5 of log10 r) / ref_dex, 0, 1) over the finite,
/// positive distances; 0 with fewer than 20 of them.
double gaia_depth_budget(const std::vector<double>& r_med, double ref_dex = 0.5);

/// nebula.structural_depth: per-pixel depth in [-1, 1] from a mono luminance (dark dust near,
/// diffuse glow far, sky below the sky_pct percentile 0), smoothed. sigma = max(4, frac*min(h,w)).
/// Throws std::invalid_argument for a multi-channel image, and (numpy would return NaN) for
/// non-finite pixels.
Image structural_depth(const Image& lum, double frac = 0.03, double sky_pct = 20.0);

/// nebula.render_nebula: amount = strength * budget (budget from the table's r_med when not
/// given); amount <= 1e-6 returns the input unchanged (not clipped). Otherwise atmospheric
/// brightness, then for colour only the saturation and red/blue tilt, then the structural pop
/// when structure > 1e-6, clipped to [0, 1].
Image render_nebula(const Image& starless, const std::vector<StarRow>& table, double strength = 1.0,
                    double atmos = 1.0, double structure = 1.0, std::optional<double> budget = std::nullopt);

}  // namespace gdg
