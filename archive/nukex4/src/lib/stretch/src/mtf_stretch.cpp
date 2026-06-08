#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <algorithm>

namespace nukex {

float MTFStretch::apply_scalar(float x) const {
    // Clip to [shadows, highlights], rescale to [0, 1]
    float range = highlights - shadows;
    if (range < 1e-10f) return 0.0f;
    float xc = std::clamp((x - shadows) / range, 0.0f, 1.0f);

    // Edge cases
    if (midtone <= 0.0f) return 0.0f;
    if (midtone >= 1.0f) return 1.0f;

    // MTF formula: ((m-1)·xc) / ((2m-1)·xc - m)
    // This maps: f(0)=0, f(1)=1, f(m)=0.5
    float m = midtone;
    return ((m - 1.0f) * xc) / ((2.0f * m - 1.0f) * xc - m);
}

void MTFStretch::apply(Image& img) const {
    auto fn = [this](float x) { return apply_scalar(x); };

    if (luminance_only) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

std::map<std::string, std::pair<float, float>> MTFStretch::param_bounds() const {
    return {
        {"midtone",    {0.0f, 1.0f}},
        {"shadows",    {0.0f, 0.1f}},
        {"highlights", {0.9f, 1.0f}},
    };
}

bool MTFStretch::set_param(const std::string& n, float v) {
    if (n == "midtone")    { midtone    = v; return true; }
    if (n == "shadows")    { shadows    = v; return true; }
    if (n == "highlights") { highlights = v; return true; }
    return false;
}

std::optional<float> MTFStretch::get_param(const std::string& n) const {
    if (n == "midtone")    return midtone;
    if (n == "shadows")    return shadows;
    if (n == "highlights") return highlights;
    return std::nullopt;
}

} // namespace nukex
