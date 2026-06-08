#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>

namespace nukex {

float LuptonStretch::apply_scalar(float x) const {
    // The Lupton asinh curve applied to a single intensity value.
    // f(I) = arcsinh(Q * I / stretch) * slope
    // where slope = frac / arcsinh(frac * Q), frac = 0.1
    if (x <= 0.0f) return 0.0f;

    constexpr float frac = 0.1f;
    float slope = frac / std::asinh(frac * Q);
    return std::asinh(Q * x / stretch) * slope;
}

void LuptonStretch::apply(Image& img) const {
    int n = img.width() * img.height();
    int nch = img.n_channels();

    if (nch < 3) {
        // Single channel: apply scalar stretch directly
        float* data = img.channel_data(0);
        for (int i = 0; i < n; i++) {
            data[i] = std::clamp(apply_scalar(data[i]), 0.0f, 1.0f);
        }
        return;
    }

    // Multi-channel Lupton RGB pipeline:
    // 1. Subtract per-band minimums
    // 2. Compute shared intensity I = (r + g + b) / 3
    // 3. Stretch intensity: f(I) = arcsinh(Q * I / stretch) * slope
    // 4. Scale all channels by f(I) / I
    // 5. Clip and handle overflow

    float* R = img.channel_data(0);
    float* G = img.channel_data(1);
    float* B = img.channel_data(2);

    constexpr float frac = 0.1f;
    float slope = frac / std::asinh(frac * Q);

    for (int i = 0; i < n; i++) {
        float r = R[i] - minimum_r;
        float g = G[i] - minimum_g;
        float b = B[i] - minimum_b;

        // Shared intensity
        float I = (r + g + b) / 3.0f;
        if (I <= 1e-10f) {
            R[i] = 0.0f;
            G[i] = 0.0f;
            B[i] = 0.0f;
            continue;
        }

        // Stretched intensity
        float fI = std::asinh(Q * I / stretch) * slope;

        // Scale ratio
        float ratio = fI / I;

        float r_out = r * ratio;
        float g_out = g * ratio;
        float b_out = b * ratio;

        // Overflow protection: if any channel exceeds 1, scale all down proportionally
        float max_val = std::max({r_out, g_out, b_out});
        if (max_val > 1.0f) {
            float inv_max = 1.0f / max_val;
            r_out *= inv_max;
            g_out *= inv_max;
            b_out *= inv_max;
        }

        R[i] = std::max(0.0f, r_out);
        G[i] = std::max(0.0f, g_out);
        B[i] = std::max(0.0f, b_out);
    }
}

std::map<std::string, std::pair<float, float>> LuptonStretch::param_bounds() const {
    return {
        {"Q",       {0.0f, 50.0f}},
        {"stretch", {0.0f, 100.0f}},
    };
}

bool LuptonStretch::set_param(const std::string& n, float v) {
    if (n == "Q")       { Q       = v; return true; }
    if (n == "stretch") { stretch = v; return true; }
    return false;
}

std::optional<float> LuptonStretch::get_param(const std::string& n) const {
    if (n == "Q")       return Q;
    if (n == "stretch") return stretch;
    return std::nullopt;
}

} // namespace nukex
