#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace nukex {

namespace {

constexpr float kFrac = 0.1f;

/// Q is the softening and stretch the linear range: Q >= 0, stretch > 0.
/// stretch is the divisor of the intensity; at 0 the map is a step that puts
/// every positive pixel at the top, so it is refused rather than evaluated.
void require_valid(float Q, float stretch) {
    if (!std::isfinite(Q) || Q < 0.0f)
        throw std::invalid_argument("Lupton: Q must be finite and >= 0 (got " + std::to_string(Q) + ")");
    if (!std::isfinite(stretch) || !(stretch > 0.0f))
        throw std::invalid_argument("Lupton: stretch must be finite and > 0 (got " + std::to_string(stretch)
                                    + "); it divides the intensity, and at 0 every positive pixel saturates");
}

/// f(I) = asinh(Q I / stretch) * frac / asinh(frac Q)   (Lupton et al. 2004),
/// with its Q -> 0 limit I / stretch: asinh(z) = z (1 - z^2/6 + ...), so for
/// small Q the ratio of the two asinh terms is (I / stretch)(1 + O(Q^2)). Below
/// Q = 1e-4 the relative difference is under 1e-8 x (1 + (I/stretch)^2), far
/// below float resolution, and the closed form would divide 0 by 0 at Q = 0.
/// (astropy's make_lupton_rgb treats Q == 0 the same way: a linear stretch.)
inline float lupton_f(float I, float Q, float stretch) {
    if (Q < 1e-4f) return I / stretch;
    return std::asinh(Q * I / stretch) * (kFrac / std::asinh(kFrac * Q));
}

} // namespace

float LuptonStretch::apply_scalar(float x) const {
    require_valid(Q, stretch);
    if (x <= 0.0f) return 0.0f;
    return lupton_f(x, Q, stretch);
}

void LuptonStretch::apply(Image& img) const {
    require_valid(Q, stretch);
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
        float fI = lupton_f(I, Q, stretch);

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
        // Q = 0 is the linear limit and valid. stretch must be > 0: 0.001 is
        // a linear range of 1/1000 of full scale, already a near-step.
        {"Q",       {0.0f, 50.0f}},
        {"stretch", {0.001f, 100.0f}},
    };
}

bool LuptonStretch::set_param(const std::string& n, float v) {
    // Refused loudly, and not applied: an invalid value is a caller's bug.
    if (n == "Q")       { require_valid(v, stretch); Q = v; return true; }
    if (n == "stretch") { require_valid(Q, v); stretch = v; return true; }
    return false;
}

std::optional<float> LuptonStretch::get_param(const std::string& n) const {
    if (n == "Q")       return Q;
    if (n == "stretch") return stretch;
    return std::nullopt;
}

} // namespace nukex
