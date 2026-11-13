// AutoContrast core (PCL-free): sRGB -> CIELAB (D65) — fingerprint/color.py, written fresh from the Python
// constants (NOT NukeX's color_composer, which uses rounded CIE constants).
#pragma once

#include "core/Image.h"

namespace ac {

// rgb_to_lab(rgb): (H, W, 3) sRGB in [0, 1] -> L*, a*, b* planes. sRGB -> linear (c <= 0.04045 ? c / 12.92 :
// ((c + 0.055) / 1.055) ** 2.4), linear @ M.T with the 7-digit IEC 61966-2-1 matrix, / D65 white
// (0.95047, 1, 1.08883), f(t) = t > (6/29)**3 ? cbrt(t) : t / (3 * (6/29)**2) + 4/29,
// L = 116 f(y) - 16, a = 500 (f(x) - f(y)), b = 200 (f(y) - f(z)). Throws std::invalid_argument unless
// rgb.channels == 3. Per-pixel work only, so `threads` (0 = default) never changes a result.
void rgb_to_lab(const Image& rgb, Plane& L, Plane& a, Plane& b, unsigned threads = 0);

} // namespace ac
