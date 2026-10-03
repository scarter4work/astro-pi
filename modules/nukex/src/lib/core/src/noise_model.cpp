#include "nukex/core/noise_model.hpp"

#include <algorithm>

namespace nukex {

float NoiseModel::sample_variance(float value, const FrameStats& fs, int slot,
                                  float fallback_var) {
    if (fs.has_noise_keywords) {
        float g = std::max(fs.gain, 1e-10f);
        float rn = fs.read_noise;

        // Undo Phase A's per-frame normalisation before the Poisson term.
        // The sample reaching us is a*raw + b; shot noise is a property of
        // the photons that were actually collected, so it must be evaluated
        // on `raw` and the result scaled back by a^2. Reading the normalised
        // value as if it were ADU would misstate the shot term by exactly the
        // factor the frame was scaled by -- the brightest frames the most.
        //
        // At the identity (a=1, b=0) every operation below is exact in
        // IEEE-754, so an un-normalised batch is bit-for-bit unmoved.
        float a = (slot >= 0 && slot < MAX_CHANNELS) ? fs.norm_scale[slot] : 1.0f;
        float b = (slot >= 0 && slot < MAX_CHANNELS) ? fs.norm_offset[slot] : 0.0f;
        if (!(a > 0.0f)) { a = 1.0f; b = 0.0f; }   // unmeasurable: identity
        float raw = (value - b) / a;

        // Convert normalized [0,1] value to ADU for CCD noise model.
        // FITS 16-bit: full range = 65535 ADU.
        float value_adu = raw * 65535.0f;
        // Poisson shot noise: variance in electrons = signal_electrons = value_adu * gain
        // Read noise: variance in electrons = rn^2
        // Total variance in ADU^2: value_adu / g + (rn / g)^2
        float shot_var_adu = value_adu / g;
        float read_var_adu = (rn * rn) / (g * g);
        // Convert back to normalized^2 units: divide by 65535^2, and forward
        // through the normalisation: Var(a*x + b) = a^2 Var(x).
        return a * a * (shot_var_adu + read_var_adu) / (65535.0f * 65535.0f);
    }
    return fallback_var;
}

} // namespace nukex
