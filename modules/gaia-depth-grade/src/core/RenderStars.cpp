#include "RenderStars.h"

#include "Numerics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gdg {

namespace {

constexpr double kPi = 3.141592653589793;  // np.pi
const double kNaN = std::numeric_limits<double>::quiet_NaN();

// np.median / MAD helpers that propagate NaN instead of throwing (numpy returns NaN).
bool any_nan(const std::vector<double>& v) {
    for (double x : v)
        if (std::isnan(x)) return true;
    return false;
}

double np_sign(double v) { return std::isnan(v) ? v : static_cast<double>((v > 0.0) - (v < 0.0)); }

// np.maximum(a, 0.0): NaN propagates.
double np_max0(double a) { return std::isnan(a) ? a : (a > 0.0 ? a : 0.0); }

// sign(hp) * maximum(|hp| - 0.75 * nz, 0) with nz = 1.4826 * median(|hp - median(hp)|): the
// noise-cored high-pass of render.py / nebula.py, over every element (all channels).
void core_highpass(std::vector<double>& hp) {
    double nz = kNaN;
    if (!any_nan(hp)) nz = 1.4826 * mad(hp);
    const double cut = 0.75 * nz;
    for (double& v : hp) v = np_sign(v) * np_max0(std::fabs(v) - cut);
}

}  // namespace

Image render_stars(const Image& stars, const std::vector<StarRow>& table, const Modulation& m, double base_sigma_px,
                   const HaloProfile* halo, double halo_gain) {
    const size_t n = table.size();
    if (m.brightness.size() != n || m.size.size() != n || m.contrast.size() != n || m.saturation.size() != n)
        throw std::invalid_argument("render_stars: modulation and table lengths differ");
    Image out = stars;
    const bool is_color = out.channels > 1;
    const int C = out.channels;
    HaloProfile fallback;
    if (!halo) {
        fallback = moffat_profile(base_sigma_px, 2.5, 8.0 * base_sigma_px);
        halo = &fallback;
    }
    const long nx = out.width, ny = out.height;
    const double core_r = std::max(base_sigma_px, 1.0);

    for (size_t i = 0; i < n; ++i) {
        const double x = table[i].x, y = table[i].y;
        if (!std::isfinite(x) || !std::isfinite(y)) throw std::invalid_argument("render_stars: non-finite star position");
        const double b = m.brightness[i], zsize = m.size[i], camount = m.contrast[i], sat = m.saturation[i];

        const long rad = static_cast<long>(std::ceil(4 * base_sigma_px * std::max(1.0, zsize)));
        const long xi = static_cast<long>(x), yi = static_cast<long>(y);  // int(): toward zero
        const long x0 = std::max(0L, xi - rad), x1 = std::min(nx, xi + rad + 1);
        const long y0 = std::max(0L, yi - rad), y1 = std::min(ny, yi + rad + 1);
        if (x1 <= x0 || y1 <= y0) continue;  // win.size == 0
        const long w = x1 - x0, h = y1 - y0;
        const double cx = x - static_cast<double>(x0), cy = y - static_cast<double>(y0);

        // The window as its own image (the Python view), written back at the end.
        Image win(static_cast<int>(w), static_cast<int>(h), C);
        for (long yy = 0; yy < h; ++yy)
            for (long xx = 0; xx < w; ++xx)
                for (int c = 0; c < C; ++c)
                    win.at(static_cast<int>(xx), static_cast<int>(yy), c) =
                        out.at(static_cast<int>(x0 + xx), static_cast<int>(y0 + yy), c);
        const size_t np_ = static_cast<size_t>(w * h);
        std::vector<double> r(np_), taper(np_);
        for (long yy = 0; yy < h; ++yy)
            for (long xx = 0; xx < w; ++xx) {
                const double dx = static_cast<double>(xx) - cx, dy = static_cast<double>(yy) - cy;
                const double rr = std::sqrt(dx * dx + dy * dy);
                const size_t q = static_cast<size_t>(yy * w + xx);
                r[q] = rr;
                double t = rr / static_cast<double>(rad);  // np.clip(r / rad, 0, 1)
                t = std::isnan(t) ? t : std::min(std::max(t, 0.0), 1.0);
                taper[q] = 0.5 * (1.0 + std::cos(kPi * t));  // Hann feather, 0 at r >= rad
            }

        // brightness: feathered multiply
        if (std::fabs(b - 1.0) > 1e-9)
            for (size_t q = 0; q < np_; ++q) {
                const double f = 1.0 + (b - 1.0) * taper[q];
                for (int c = 0; c < C; ++c) win.data[q * C + c] *= f;
            }

        // size: one-sided star-shine halo (enlarge only)
        if (zsize > 1.0 + 1e-9) {
            std::vector<double> rs(np_);
            for (size_t q = 0; q < np_; ++q) rs[q] = r[q] / zsize;
            std::vector<double> shape = halo->sample(rs);
            for (size_t q = 0; q < np_; ++q) shape[q] = shape[q] * taper[q];
            std::vector<double> lum(np_);
            for (size_t q = 0; q < np_; ++q) lum[q] = is_color ? pairwise_sum(win.data.data() + q * C, C) / C : win.data[q];
            // peak_core = lum[core].max() if core.any() else lum.max() (NaN propagates)
            bool any_core = false;
            double peak_core = -std::numeric_limits<double>::infinity(), peak_all = peak_core;
            bool nan_core = false, nan_all = false;
            for (size_t q = 0; q < np_; ++q) {
                if (std::isnan(lum[q])) nan_all = true;
                else peak_all = std::max(peak_all, lum[q]);
                if (r[q] <= core_r) {
                    any_core = true;
                    if (std::isnan(lum[q])) nan_core = true;
                    else peak_core = std::max(peak_core, lum[q]);
                }
            }
            double peak = any_core ? (nan_core ? kNaN : peak_core) : (nan_all ? kNaN : peak_all);
            const double extra = halo_gain * (zsize - 1.0) * peak;
            if (is_color) {
                // _core_color: per-channel mean over the core (np.mean(axis=0): sequential sum
                // down the rows), normalised so the brightest channel is 1.
                std::vector<double> color(static_cast<size_t>(C), 1.0);
                if (any_core) {
                    std::vector<double> sum(static_cast<size_t>(C), 0.0);
                    size_t k = 0;
                    for (size_t q = 0; q < np_; ++q)
                        if (r[q] <= core_r) {
                            for (int c = 0; c < C; ++c) sum[static_cast<size_t>(c)] += win.data[q * C + c];
                            ++k;
                        }
                    double mx = -std::numeric_limits<double>::infinity();
                    bool nan = false;
                    for (int c = 0; c < C; ++c) {
                        sum[static_cast<size_t>(c)] /= static_cast<double>(k);
                        if (std::isnan(sum[static_cast<size_t>(c)])) nan = true;
                        else mx = std::max(mx, sum[static_cast<size_t>(c)]);
                    }
                    if (nan) mx = kNaN;
                    if (mx > 0.0)
                        for (int c = 0; c < C; ++c) color[static_cast<size_t>(c)] = sum[static_cast<size_t>(c)] / mx;
                }
                for (size_t q = 0; q < np_; ++q) {
                    const double a = extra * shape[q];
                    for (int c = 0; c < C; ++c) win.data[q * C + c] += a * color[static_cast<size_t>(c)];
                }
            } else {
                for (size_t q = 0; q < np_; ++q) win.data[q] += extra * shape[q];
            }
        }

        // local contrast: noise-cored unsharp, feathered
        if (std::fabs(camount) > 1e-9) {
            const Image blur = gaussian_filter(win, base_sigma_px);
            std::vector<double> hp(win.data.size());
            for (size_t k = 0; k < hp.size(); ++k) hp[k] = win.data[k] - blur.data[k];
            core_highpass(hp);
            for (size_t q = 0; q < np_; ++q) {
                const double a = camount * taper[q];
                for (int c = 0; c < C; ++c) win.data[q * C + c] += a * hp[q * C + c];
            }
        }

        // saturation (colour only), feathered
        if (is_color && std::fabs(sat - 1.0) > 1e-9)
            for (size_t q = 0; q < np_; ++q) {
                double* p = win.data.data() + q * C;
                const double lum = pairwise_sum(p, C) / C;
                const double f = 1.0 + (sat - 1.0) * taper[q];
                for (int c = 0; c < C; ++c) p[c] = lum + f * (p[c] - lum);
            }

        for (long yy = 0; yy < h; ++yy)
            for (long xx = 0; xx < w; ++xx)
                for (int c = 0; c < C; ++c)
                    out.at(static_cast<int>(x0 + xx), static_cast<int>(y0 + yy), c) =
                        win.at(static_cast<int>(xx), static_cast<int>(yy), c);
    }

    for (double& v : out.data)  // np.clip(out, 0, 1): NaN stays NaN
        if (!std::isnan(v)) v = std::min(std::max(v, 0.0), 1.0);
    return out;
}

}  // namespace gdg
