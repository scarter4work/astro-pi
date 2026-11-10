#include "Nebula.h"

#include "Numerics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace gdg {

namespace {

double clip(double v, double lo, double hi) { return std::isnan(v) ? v : std::min(std::max(v, lo), hi); }

void require_finite(const Image& im, const char* fn) {
    for (double v : im.data)
        if (!std::isfinite(v)) throw std::invalid_argument(std::string(fn) + ": non-finite pixel");
}

}  // namespace

double gaia_depth_budget(const std::vector<double>& r_med, double ref_dex) {
    std::vector<double> logr;
    for (double r : r_med)
        if (std::isfinite(r) && r > 0.0) logr.push_back(std::log10(r));
    if (logr.size() < 20) return 0.0;
    const std::vector<double> p = percentiles(logr, {5.0, 95.0});
    return clip((p[1] - p[0]) / ref_dex, 0.0, 1.0);
}

Image structural_depth(const Image& lum, double frac, double sky_pct) {
    if (lum.channels != 1) throw std::invalid_argument("structural_depth: mono luminance required");
    require_finite(lum, "structural_depth");
    const double sigma = std::max(4.0, frac * static_cast<double>(std::min(lum.height, lum.width)));
    const Image bg = gaussian_filter(lum, sigma);
    std::vector<double> resid(lum.data.size());
    for (size_t i = 0; i < resid.size(); ++i) resid[i] = lum.data[i] - bg.data[i];
    const double scale = 1.4826 * mad(resid) + 1e-6;
    const double floor = percentile(lum.data, sky_pct);
    const double span = percentile(lum.data, 60.0) - floor + 1e-6;
    Image s(lum.width, lum.height, 1);
    for (size_t i = 0; i < resid.size(); ++i) {
        const double d = clip(-resid[i] / (3.0 * scale), -1.0, 1.0);    // dark dust -> +near
        const double signal = clip((lum.data[i] - floor) / span, 0.0, 1.0);  // 0 over sky, 1 over nebula
        s.data[i] = d * signal;
    }
    return gaussian_filter(s, sigma * 0.5);
}

Image render_nebula(const Image& starless, const std::vector<StarRow>& table, double strength, double atmos,
                    double structure, std::optional<double> budget) {
    Image out = starless;
    const bool is_color = out.channels > 1;
    const size_t C = static_cast<size_t>(out.channels);
    if (!budget) {
        std::vector<double> r;
        r.reserve(table.size());
        for (const StarRow& row : table) r.push_back(row.r_med);
        budget = gaia_depth_budget(r);
    }
    const double amt = strength * *budget;
    if (amt <= 1e-6) return out;

    const Image s = structural_depth(luminance(out));
    const size_t n = s.data.size();

    // 1) atmospheric brightness: near brighter, far dimmer
    const double k1 = 0.18 * atmos * amt;
    for (size_t q = 0; q < n; ++q) {
        const double f = 1.0 + k1 * s.data[q];
        for (size_t c = 0; c < C; ++c) out.data[q * C + c] *= f;
    }

    // 2) atmospheric colour (colour only): saturation with depth, then a gentle red/blue tilt
    if (is_color) {
        const double k2 = 0.15 * atmos * amt, k3 = 0.025 * atmos * amt;
        for (size_t q = 0; q < n; ++q) {
            double* p = out.data.data() + q * C;
            const double lum = pairwise_sum(p, C) / static_cast<double>(C);
            const double f = 1.0 + k2 * s.data[q];
            for (size_t c = 0; c < C; ++c) p[c] = lum + f * (p[c] - lum);
            const double tilt = k3 * s.data[q];
            p[0] *= 1.0 + tilt;              // red / Ha lifts toward the viewer
            p[C - 1] *= 1.0 - 0.5 * tilt;    // blue / OIII cools into the distance
        }
    }

    // 3) structural pop: depth-weighted, noise-cored local contrast
    if (structure > 1e-6) {
        const Image blur = gaussian_filter(out, 2.0);
        std::vector<double> hp(out.data.size());
        for (size_t k = 0; k < hp.size(); ++k) hp[k] = out.data[k] - blur.data[k];
        const double nz = 1.4826 * mad(hp);
        const double cut = 0.75 * nz;
        for (double& v : hp) {
            const double sgn = static_cast<double>((v > 0.0) - (v < 0.0));
            const double a = std::fabs(v) - cut;
            v = sgn * (a > 0.0 ? a : 0.0);
        }
        const double k4 = 0.45 * structure * amt;
        for (size_t q = 0; q < n; ++q) {
            const double a = k4 * s.data[q];
            for (size_t c = 0; c < C; ++c) out.data[q * C + c] += a * hp[q * C + c];
        }
    }

    for (double& v : out.data) v = clip(v, 0.0, 1.0);
    return out;
}

}  // namespace gdg
