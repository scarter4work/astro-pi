// Ionization ratio map, the OIII fraction as a depth coordinate: a port of recon/ionization.py.
#pragma once

#include "Model.h"

namespace gdg::recon {

/// ionization.py _SMOOTH_SIGMA_PX.
inline constexpr double IONIZATION_SMOOTH_SIGMA_PX = 2.0;

/// ionization_ratio(ch, eps): with ha, oiii = clip(nan_to_num(float64(.)), 0, None),
/// clip(G(oiii) / (G(oiii + ha) + eps), 0, 1) as float32, G = gaussian_filter(sigma 2). The
/// confidence is ch.ratio_confidence; sii does not alter the map (None is fully supported).
IonizationMap ionization_ratio(const ChannelSet& ch, double eps = 1e-6);

}  // namespace gdg::recon
