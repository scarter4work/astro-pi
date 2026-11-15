#pragma once

#include "nukex/io/image.hpp"
#include <algorithm>
#include <cstddef>
#include <functional>

namespace nukex {

/// Apply a scalar stretch function to an image's luminance only,
/// preserving chromaticity (hue + saturation).
///
/// For single-channel images: applies directly.
/// For multi-channel (RGB): compute luminance L = 0.2126R + 0.7152G + 0.0722B,
/// stretch L, scale R/G/B by L'/L ratio.
///
/// Reference: Rec. 709 luminance coefficients.
void apply_luminance_only(Image& img, const std::function<float(float)>& fn);

/// Batched form of apply_luminance_only(): `fn(const float* in, float* out,
/// std::size_t n)` stretches a block of luminance values (in == out never
/// happens here). Same per-pixel arithmetic, so the result is bit-identical to
/// apply_luminance_only() with the equivalent scalar function, but the
/// stretch runs over contiguous blocks the compiler can vectorise.
template <class SpanFn>
void apply_luminance_only_span(Image& img, SpanFn&& fn) {
    const int nch = img.n_channels();
    const std::size_t n = static_cast<std::size_t>(img.width()) * img.height();
    if (nch == 1) {
        fn(img.channel_data(0), img.channel_data(0), n);
        return;
    }
    constexpr float kR = 0.2126f, kG = 0.7152f, kB = 0.0722f;
    constexpr std::size_t kBlock = 4096;
    float* r = img.channel_data(0);
    float* g = img.channel_data(1);
    float* b = (nch > 2) ? img.channel_data(2) : nullptr;
    float L[kBlock], Ls[kBlock];
    for (std::size_t base = 0; base < n; base += kBlock) {
        const std::size_t cnt = std::min(kBlock, n - base);
        for (std::size_t k = 0; k < cnt; ++k) {
            const std::size_t i = base + k;
            const float bv = b ? b[i] : r[i];
            L[k] = kR * r[i] + kG * g[i] + kB * bv;
        }
        fn(static_cast<const float*>(L), static_cast<float*>(Ls), cnt);
        for (std::size_t k = 0; k < cnt; ++k) {
            if (L[k] < 1e-10f) continue;  // Black pixel -- nothing to scale
            const std::size_t i = base + k;
            const float scale = Ls[k] / L[k];
            r[i] = std::clamp(r[i] * scale, 0.0f, 1.0f);
            g[i] = std::clamp(g[i] * scale, 0.0f, 1.0f);
            if (b) b[i] = std::clamp(b[i] * scale, 0.0f, 1.0f);
        }
    }
}

/// Apply a scalar function per-channel independently.
void apply_per_channel(Image& img, const std::function<float(float)>& fn);

/// Clamp all pixel values in an image to [lo, hi].
void clamp_image(Image& img, float lo = 0.0f, float hi = 1.0f);

} // namespace nukex
