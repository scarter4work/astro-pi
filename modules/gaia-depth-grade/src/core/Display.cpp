#include "Display.h"

#include "Numerics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gdg {

namespace {

double clip(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

}  // namespace

Image screen_blend(const Image& a, const Image& b) {
    if (!a.same_shape(b)) throw std::invalid_argument("screen_blend: image shapes differ");
    Image out = a;
    for (size_t i = 0; i < out.data.size(); ++i) out.data[i] = 1.0 - (1.0 - a.data[i]) * (1.0 - b.data[i]);
    return out;
}

std::vector<uint8_t> autostretch_u8(const Image& rgb, double target_bg, double shadows_clip) {
    if (rgb.data.empty()) throw std::invalid_argument("autostretch_u8: empty image");
    std::vector<double> x(rgb.data.size());
    for (size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(rgb.data[i])) throw std::invalid_argument("autostretch_u8: non-finite pixel");
        x[i] = std::max(rgb.data[i], 0.0);  // np.clip(rgb, 0, None)
    }
    const double med = median(x);
    const double mad_ = mad(x) * 1.4826;
    const double shadows = clip(med + shadows_clip * mad_, 0.0, 1.0);
    const double denom = std::max(1e-8, 1.0 - shadows);
    const double r = (med - shadows) / denom;
    const double B = target_bg;
    const double sden = (2.0 * B * r - B - r);
    double m = std::fabs(sden) < 1e-12 ? 0.5 : (r * (B - 1.0)) / sden;
    m = clip(m, 1e-3, 1.0 - 1e-3);
    std::vector<uint8_t> out(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        const double y = clip((x[i] - shadows) / denom, 0.0, 1.0);
        const double den = (2.0 * m - 1.0) * y - m;  // PixInsight MTF
        const double v = clip(den == 0.0 ? y : ((m - 1.0) * y) / den, 0.0, 1.0);
        out[i] = static_cast<uint8_t>(v * 255.0 + 0.5);  // astype(uint8): truncation
    }
    return out;
}

Image crop(const Image& img, int x, int y, int w, int h) {
    // Python slice bounds: negative starts count from the end; clamp to [0, n].
    auto norm = [](long v, long n) {
        if (v < 0) v += n;
        return std::clamp(v, 0L, n);
    };
    const long x0 = norm(x, img.width), y0 = norm(y, img.height);
    const long x1 = std::max(x0, norm(static_cast<long>(x) + w, img.width));
    const long y1 = std::max(y0, norm(static_cast<long>(y) + h, img.height));
    Image out(static_cast<int>(x1 - x0), static_cast<int>(y1 - y0), img.channels);
    for (long yy = y0; yy < y1; ++yy)
        for (long xx = x0; xx < x1; ++xx)
            for (int c = 0; c < img.channels; ++c)
                out.at(static_cast<int>(xx - x0), static_cast<int>(yy - y0), c) =
                    img.at(static_cast<int>(xx), static_cast<int>(yy), c);
    return out;
}

std::pair<int, int> downscale_size(int w, int h, int max_width) {
    if (max_width < 1 || w <= max_width) return {w, h};
    const double scale = static_cast<double>(max_width) / static_cast<double>(w);
    const int nh = static_cast<int>(std::nearbyint(static_cast<double>(h) * scale));
    return {max_width, std::max(1, nh)};
}

std::vector<uint8_t> downscale_u8(const std::vector<uint8_t>& rgb, int w, int h, int channels, int max_width) {
    if (w < 0 || h < 0 || channels < 1 ||
        rgb.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(channels))
        throw std::invalid_argument("downscale_u8: buffer size does not match w * h * channels");
    const auto [ow, oh] = downscale_size(w, h, max_width);
    if (ow == w && oh == h) return rgb;
    std::vector<uint8_t> out(static_cast<size_t>(ow) * oh * channels);
    for (int oy = 0; oy < oh; ++oy) {
        const long sy0 = static_cast<long>(oy) * h / oh;
        const long sy1 = std::max(sy0 + 1, static_cast<long>(oy + 1) * h / oh);
        for (int ox = 0; ox < ow; ++ox) {
            const long sx0 = static_cast<long>(ox) * w / ow;
            const long sx1 = std::max(sx0 + 1, static_cast<long>(ox + 1) * w / ow);
            const long count = (sy1 - sy0) * (sx1 - sx0);
            for (int c = 0; c < channels; ++c) {
                long sum = 0;
                for (long y = sy0; y < sy1; ++y)
                    for (long x = sx0; x < sx1; ++x) sum += rgb[(static_cast<size_t>(y) * w + x) * channels + c];
                out[(static_cast<size_t>(oy) * ow + ox) * channels + c] = static_cast<uint8_t>((sum + count / 2) / count);
            }
        }
    }
    return out;
}

}  // namespace gdg
