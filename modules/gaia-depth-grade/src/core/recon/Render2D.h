// The CavityModel rendered back onto the image grid as a registered linear RGB image (recon Part 2,
// native; PCL-free). The render only attenuates or limb-brightens the model's own linear
// emissivity: it never stretches the data, and a flat, dust-free model returns clip(E, 0, 1).
#pragma once

#include "Model.h"

namespace gdg::recon {

/// Percentile of the non-zero depth-gradient magnitudes that maps to the full limb gain.
inline constexpr double RENDER_LIMB_NORM_PCT = 99.5;
/// Gaussian sigma (px) applied to z before its gradient is taken.
inline constexpr double RENDER_LIMB_SIGMA_PX = 1.0;

/// render_cavity_2d(model, cfg): (ny, nx, 3) float32-valued image in [0, 1] on the model's grid.
/// With a = render_ambient, s = render_depth_shading, gamma = render_gamma, per pixel:
///   z_rel = z - min(z)
///   shade = (a + (1 - a) clip(1 - s z_rel, 0, 1))^gamma
///   g     = |grad gaussian_filter(z, 1)|  (np.gradient: central inside, one-sided at the edges;
///           an axis of length 1 has zero gradient)
///   q     = the 99.5th percentile of the g > 0 values (no limb term when there are none)
///   limb  = 1 + render_limb_gain clip(g / q, 0, 1)
///   T     = 1 - clip(render_dust_strength dust, 0, 1)
///   out_c = clip(f32(E_c shade limb T), 0, 1)
/// Throws std::invalid_argument unless emissivity has 3 channels, z and dust are planes of one shape
/// and every z / emissivity / dust value is finite (the message names the plane and the pixel).
Image render_cavity_2d(const CavityModel& model, const ReconConfig& cfg);

}  // namespace gdg::recon
