#include "Display.h"

#include "Numerics.h"
#include "Parallel.h"

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

StfParams stf_params(const Image& rgb, double target_bg, double shadows_clip) {
    if (rgb.data.empty()) throw std::invalid_argument("stf_params: empty image");
    std::vector<double> x(rgb.data.size());
    for (size_t i = 0; i < x.size(); ++i) {
        if (!std::isfinite(rgb.data[i])) throw std::invalid_argument("stf_params: non-finite pixel");
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
    const size_t nc = static_cast<size_t>(rgb.channels);
    return StfParams{std::vector<double>(nc, shadows), std::vector<double>(nc, m)};
}

std::vector<uint8_t> apply_stf_u8(const Image& rgb, const StfParams& params) {
    const size_t nc = static_cast<size_t>(rgb.channels);
    if (params.c0.size() != nc || params.m.size() != nc)
        throw std::invalid_argument("apply_stf_u8: the STF parameters must hold one entry per channel");
    for (size_t c = 0; c < nc; ++c)
        if (!(std::isfinite(params.c0[c]) && params.c0[c] >= 0.0 && params.c0[c] <= 1.0 && std::isfinite(params.m[c]) &&
              params.m[c] > 0.0 && params.m[c] < 1.0))
            throw std::invalid_argument("apply_stf_u8: STF parameters out of range (c0 in [0, 1], m in (0, 1))");
    std::vector<uint8_t> out(rgb.data.size());
    const size_t row = static_cast<size_t>(rgb.width) * nc;
    // Elementwise: rows in parallel give the serial bytes. The first non-finite pixel's exception
    // is rethrown by parallel_for_if (every message is the same).
    parallel_for_if(static_cast<long long>(rgb.data.size()) * 8, rgb.height, [&](int y) {
        const size_t begin = static_cast<size_t>(y) * row;
        for (size_t i = begin; i < begin + row; ++i) {
            const size_t c = (i - begin) % nc;
            const double shadows = params.c0[c], m = params.m[c];
            const double denom = std::max(1e-8, 1.0 - shadows);
            if (!std::isfinite(rgb.data[i])) throw std::invalid_argument("apply_stf_u8: non-finite pixel");
            const double xv = std::max(rgb.data[i], 0.0);  // np.clip(rgb, 0, None)
            const double yv = clip((xv - shadows) / denom, 0.0, 1.0);
            const double den = (2.0 * m - 1.0) * yv - m;  // PixInsight MTF
            const double v = clip(den == 0.0 ? yv : ((m - 1.0) * yv) / den, 0.0, 1.0);
            out[i] = static_cast<uint8_t>(v * 255.0 + 0.5);  // astype(uint8): truncation
        }
    });
    return out;
}

std::vector<uint8_t> autostretch_u8(const Image& rgb, double target_bg, double shadows_clip) {
    return apply_stf_u8(rgb, stf_params(rgb, target_bg, shadows_clip));
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

std::pair<int, int> fit_size(int w, int h, int max_width, int max_height) {
    if (w < 1 || h < 1) throw std::invalid_argument("fit_size: the image has no pixels");
    if (max_height < 1) return downscale_size(w, h, max_width);
    const double sw = (max_width < 1 || w <= max_width) ? 1.0 : static_cast<double>(max_width) / w;
    const double sh = h <= max_height ? 1.0 : static_cast<double>(max_height) / h;
    if (sw >= 1.0 && sh >= 1.0) return {w, h};
    if (sw <= sh) return downscale_size(w, h, max_width);  // width-bound
    const int nw = static_cast<int>(std::nearbyint(static_cast<double>(w) * sh));
    return {std::max(1, nw), max_height};
}

std::vector<uint8_t> downscale_u8(const std::vector<uint8_t>& rgb, int w, int h, int channels, int max_width) {
    const auto [ow, oh] = downscale_size(w, h, max_width);
    return downscale_u8_to(rgb, w, h, channels, ow, oh);
}

std::vector<uint8_t> downscale_u8_to(const std::vector<uint8_t>& rgb, int w, int h, int channels, int ow, int oh) {
    if (w < 1 || h < 1) throw std::invalid_argument("downscale_u8: the image has no pixels");
    if (channels < 1 || rgb.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(channels))
        throw std::invalid_argument("downscale_u8: buffer size does not match w * h * channels");
    if (ow < 1 || oh < 1 || ow > w || oh > h)
        throw std::invalid_argument("downscale_u8: the target size must be within the source size, at least 1 x 1");
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
