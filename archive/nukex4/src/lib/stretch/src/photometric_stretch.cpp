#include "nukex/stretch/photometric_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

float PhotometricStretch::apply_scalar(float x) const {
    if (x <= 0.0f) return 0.0f;

    // Convert normalized pixel value back to flux
    // The FITS reader normalized by max, so x is in [0, 1].
    // We need to convert to e-/s/arcsec² for the magnitude computation.
    //
    // flux_per_pixel = x * max_adu * gain / exptime   (e-/s per pixel)
    // flux_per_arcsec2 = flux_per_pixel / plate_scale²
    //
    // Since we only have normalized [0,1] data and the original max_adu is lost,
    // we fold everything into the zero point:
    //   mu = -2.5 * log10(x * gain / exptime) + zp + 2.5 * log10(plate_scale²)
    //      = -2.5 * log10(x) + [-2.5*log10(gain/exptime) + zp + 5*log10(plate_scale)]
    //
    // The bracketed term is a combined zero point that absorbs the unknown max_adu.
    // We call it zp_eff.

    float zp_eff = -2.5f * std::log10(gain / exptime) + zp + 5.0f * std::log10(plate_scale);
    float mu = -2.5f * std::log10(x) + zp_eff;

    // Map surface brightness to display [0, 1]
    // mu_bright → 1.0 (bright), mu_faint → 0.0 (dark)
    float display = (mu_faint - mu) / (mu_faint - mu_bright);
    return std::clamp(display, 0.0f, 1.0f);
}

void PhotometricStretch::apply(Image& img) const {
    // Auto-estimate sky level if not provided
    float sky = sky_level;
    if (sky <= 0.0f) {
        int n = img.width() * img.height();
        std::vector<float> vals;
        if (img.n_channels() >= 3) {
            vals.resize(n);
            for (int i = 0; i < n; i++)
                vals[i] = 0.2126f * img.channel_data(0)[i]
                        + 0.7152f * img.channel_data(1)[i]
                        + 0.0722f * img.channel_data(2)[i];
        } else {
            vals.assign(img.channel_data(0), img.channel_data(0) + n);
        }
        std::nth_element(vals.begin(), vals.begin() + n / 2, vals.end());
        sky = vals[n / 2];
    }

    // Apply: subtract sky, then photometric mapping
    auto fn = [this, sky](float x) -> float {
        float signal = x - sky;
        if (signal <= 0.0f) return 0.0f;
        return apply_scalar(signal);
    };

    if (luminance_only && img.n_channels() >= 3) {
        apply_luminance_only(img, fn);
    } else {
        apply_per_channel(img, fn);
    }
    clamp_image(img);
}

} // namespace nukex
