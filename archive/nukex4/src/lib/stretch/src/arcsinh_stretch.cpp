#include "nukex/stretch/arcsinh_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>

namespace nukex {

float ArcSinhStretch::apply_scalar(float x) const {
    if (alpha < 1e-10f) return x;  // Identity when alpha ≈ 0
    return std::asinh(alpha * x) / std::asinh(alpha);
}

void ArcSinhStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

std::map<std::string, std::pair<float, float>> ArcSinhStretch::param_bounds() const {
    return {
        {"alpha", {1.0f, 10000.0f}},
    };
}

bool ArcSinhStretch::set_param(const std::string& n, float v) {
    if (n == "alpha") { alpha = v; return true; }
    return false;
}

std::optional<float> ArcSinhStretch::get_param(const std::string& n) const {
    if (n == "alpha") return alpha;
    return std::nullopt;
}

} // namespace nukex
