#include "nukex/stretch/log_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>

namespace nukex {

float LogStretch::apply_scalar(float x) const {
    if (alpha < 1e-10f) return x;
    return std::log1p(alpha * x) / std::log1p(alpha);
}

void LogStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

std::map<std::string, std::pair<float, float>> LogStretch::param_bounds() const {
    return {
        {"alpha", {1.0f, 100000.0f}},
    };
}

bool LogStretch::set_param(const std::string& n, float v) {
    if (n == "alpha") { alpha = v; return true; }
    return false;
}

std::optional<float> LogStretch::get_param(const std::string& n) const {
    if (n == "alpha") return alpha;
    return std::nullopt;
}

} // namespace nukex
