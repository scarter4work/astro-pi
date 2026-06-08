#include "nukex/stretch/rnc_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>

namespace nukex {

float RNCStretch::apply_scalar(float x) const {
    float range = white_point - black_point;
    if (range < 1e-10f) return 0.0f;
    float xc = std::clamp((x - black_point) / range, 0.0f, 1.0f);
    if (gamma < 1e-10f) return (xc > 0.0f) ? 1.0f : 0.0f;
    return std::pow(xc, 1.0f / gamma);
}

void RNCStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

} // namespace nukex
