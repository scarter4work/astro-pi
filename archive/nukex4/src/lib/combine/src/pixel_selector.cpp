#include "nukex/combine/pixel_selector.hpp"
#include <cmath>
#include <algorithm>

namespace nukex {

float PixelSelector::sample_variance(float value, const FrameStats& fs,
                                      float welford_var) {
    if (fs.has_noise_keywords) {
        float g = std::max(fs.gain, 1e-10f);
        float rn = fs.read_noise;
        // Convert normalized [0,1] value to ADU for CCD noise model.
        // FITS 16-bit: full range = 65535 ADU.
        float value_adu = value * 65535.0f;
        // Poisson shot noise: variance in electrons = signal_electrons = value_adu * gain
        // Read noise: variance in electrons = rn^2
        // Total variance in ADU^2: value_adu / g + (rn / g)^2
        float shot_var_adu = value_adu / g;
        float read_var_adu = (rn * rn) / (g * g);
        // Convert back to normalized^2 units: divide by 65535^2
        return (shot_var_adu + read_var_adu) / (65535.0f * 65535.0f);
    }
    return welford_var;
}

void PixelSelector::select(const ZDistribution& dist,
                            const float* values, const float* weights, int n,
                            const FrameStats* frame_stats, const int* frame_indices,
                            float welford_var,
                            float& out_value, float& out_noise, float& out_snr) const {
    out_value = dist.true_signal_estimate;

    double weight_sum = 0.0;
    double variance_sum = 0.0;

    for (int i = 0; i < n; i++) {
        double w = static_cast<double>(weights[i]);
        int fi = frame_indices[i];
        float sigma2 = sample_variance(values[i], frame_stats[fi], welford_var);
        weight_sum += w;
        variance_sum += w * w * static_cast<double>(sigma2);
    }

    if (weight_sum > 1e-30) {
        out_noise = static_cast<float>(std::sqrt(variance_sum) / weight_sum);
    } else {
        out_noise = 0.0f;
    }

    if (out_noise > 1e-30f) {
        out_snr = std::clamp(out_value / out_noise, 0.0f, 9999.0f);
    } else {
        out_snr = 0.0f;
    }
}

} // namespace nukex
