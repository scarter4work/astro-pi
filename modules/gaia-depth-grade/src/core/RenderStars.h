// Per-star depth render, as the Python core's render.py (PCL-free, reentrant).
#pragma once

#include "Detect.h"
#include "HaloProfile.h"
#include "Image.h"
#include "Modulate.h"

#include <vector>

namespace gdg {

/// render.render_stars: for each table row IN ORDER (overlapping windows see the earlier stars'
/// edits), on a window of radius ceil(4 * base_sigma * max(1, size)) clipped to the frame with
/// int() truncation, under a Hann taper: brightness feather; one-sided star-shine halo when
/// size > 1 (shape halo->sample(r / size), amplitude halo_gain * (size - 1) * core peak, tinted
/// by the core colour); noise-cored unsharp (gaussian_filter of the window crop, coring at
/// 0.75 * 1.4826 * MAD); colour-only saturation; then the whole image clipped to [0, 1].
/// `halo` null means moffat_profile(base_sigma_px, 2.5, 8 * base_sigma_px). Throws
/// std::invalid_argument when the modulation length differs from the table's or a row's x/y is
/// not finite (Python's int() raises there).
Image render_stars(const Image& stars, const std::vector<StarRow>& table, const Modulation& m, double base_sigma_px,
                   const HaloProfile* halo = nullptr, double halo_gain = 1.0);

}  // namespace gdg
