#include "nukex/stretch/ots_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace nukex {

// ── Target quantile functions ──
// F_target_inv(p): given a probability p ∈ [0,1], return the
// corresponding value in the target distribution.

float OTSStretch::target_quantile(float p) const {
    p = std::clamp(p, 0.0f, 1.0f);

    switch (target) {
    case OTSTarget::SQRT:
        // F(x) = sqrt(x), F_inv(p) = p^2
        return p * p;

    case OTSTarget::UNIFORM:
        // F(x) = x, F_inv(p) = p
        return p;

    case OTSTarget::GAUSSIAN: {
        // Approximate the Gaussian quantile (inverse normal CDF)
        // Using rational approximation (Abramowitz & Stegun 26.2.23)
        if (p <= 0.0f) return 0.0f;
        if (p >= 1.0f) return 1.0f;

        // Standard normal quantile
        float z;
        if (p < 0.5f) {
            float t = std::sqrt(-2.0f * std::log(p));
            z = -(t - (2.515517f + t * (0.802853f + t * 0.010328f)) /
                       (1.0f + t * (1.432788f + t * (0.189269f + t * 0.001308f))));
        } else {
            float t = std::sqrt(-2.0f * std::log(1.0f - p));
            z = t - (2.515517f + t * (0.802853f + t * 0.010328f)) /
                     (1.0f + t * (1.432788f + t * (0.189269f + t * 0.001308f)));
        }

        // Transform to target: x = mu + sigma * z, clamp to [0,1]
        return std::clamp(gauss_mu + gauss_sigma * z, 0.0f, 1.0f);
    }

    case OTSTarget::MUNSELL:
    default: {
        // CIE L* inverse: given L* (scaled to [0,1] as p), compute Y
        // L* = 116 * (Y)^(1/3) - 16  for Y > (6/29)^3
        // L* = (29/3)^3 * Y           for Y <= (6/29)^3
        // We use p as L*/100, so L* = 100*p
        float Lstar = 100.0f * p;
        float Y;
        if (Lstar > 8.0f) {
            float t = (Lstar + 16.0f) / 116.0f;
            Y = t * t * t;
        } else {
            Y = Lstar * 3.0f * 3.0f * 3.0f / (29.0f * 29.0f * 29.0f);
            // = Lstar / 903.3
        }
        return std::clamp(Y, 0.0f, 1.0f);
    }
    }
}

std::vector<float> OTSStretch::build_lut(const float* data, int n) const {
    // Step 1: Build source histogram
    std::vector<int> hist(n_bins, 0);
    for (int i = 0; i < n; i++) {
        int bin = static_cast<int>(std::clamp(data[i], 0.0f, 1.0f) * (n_bins - 1));
        hist[bin]++;
    }

    // Step 2: Build source CDF
    std::vector<float> cdf(n_bins);
    float inv_n = 1.0f / static_cast<float>(n);
    int cumsum = 0;
    for (int i = 0; i < n_bins; i++) {
        cumsum += hist[i];
        cdf[i] = cumsum * inv_n;
    }

    // Step 3: Build LUT: for each bin, map through target quantile
    std::vector<float> lut(n_bins);
    for (int i = 0; i < n_bins; i++) {
        lut[i] = target_quantile(cdf[i]);
    }

    return lut;
}

void OTSStretch::apply(Image& img) const {
    if (luminance_only && img.n_channels() > 1) {
        // Compute luminance
        int n = img.width() * img.height();
        std::vector<float> lum(n);
        for (int i = 0; i < n; i++) {
            float r = img.channel_data(0)[i];
            float g = img.channel_data(1)[i];
            float b = img.channel_data(std::min(2, img.n_channels() - 1))[i];
            lum[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        // Build LUT from luminance
        auto lut = build_lut(lum.data(), n);
        float inv_bins = static_cast<float>(n_bins - 1);

        // Apply: stretch luminance, scale RGB by L'/L
        for (int i = 0; i < n; i++) {
            float L = lum[i];
            if (L < 1e-10f) continue;

            int bin = static_cast<int>(std::clamp(L, 0.0f, 1.0f) * inv_bins);
            float L_new = lut[bin];
            float scale = L_new / L;

            for (int c = 0; c < img.n_channels(); c++) {
                img.channel_data(c)[i] = std::clamp(
                    img.channel_data(c)[i] * scale, 0.0f, 1.0f);
            }
        }
    } else {
        // Per-channel
        for (int c = 0; c < img.n_channels(); c++) {
            int n = img.width() * img.height();
            auto lut = build_lut(img.channel_data(c), n);
            float inv_bins = static_cast<float>(n_bins - 1);
            float* data = img.channel_data(c);
            for (int i = 0; i < n; i++) {
                int bin = static_cast<int>(std::clamp(data[i], 0.0f, 1.0f) * inv_bins);
                data[i] = lut[bin];
            }
        }
    }
    clamp_image(img);
}

} // namespace nukex
