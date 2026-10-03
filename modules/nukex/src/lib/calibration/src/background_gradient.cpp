#include "nukex/calibration/background_gradient.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nukex {

namespace {

// A tilt is a large-scale property, so a decimated sample describes it as well
// as every pixel would and costs a fraction as much on a 24 MP frame.
constexpr int   SAMPLE_STEP     = 8;
constexpr int   MAX_ITERATIONS  = 12;
// Asymmetric clipping. Astronomical objects are POSITIVE excursions, so cutting
// positive residuals hard and negative ones loosely settles the plane onto the
// sky rather than onto the sky plus whatever is sitting on it.
constexpr float CLIP_HIGH_SIGMA = 1.0f;
constexpr float CLIP_LOW_SIGMA  = 3.0f;
// The seed is the darkest fraction of the samples. Sky is never brighter than
// what sits on it, so this is sky plus at most the faintest part of an object,
// which the first residual clip then removes. Seeding from ALL samples instead
// lets a bright object drag the initial plane so far that the residuals become
// bimodal and their scatter stops discriminating; measured, that failed at 30%
// object coverage. 15% keeps the seed pure sky up to ~85% object coverage;
// beyond that there is no sky to fit and no automatic method is safe, which is
// what the user's switch is for.
constexpr float SEED_QUANTILE   = 0.15f;
constexpr std::size_t SEED_MIN  = 48;      // small test images still get a fit

float median_of(std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    const std::size_t k = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

float quantile_of(std::vector<float>& v, float q) {
    if (v.empty()) return 0.0f;
    std::size_t k = static_cast<std::size_t>(q * static_cast<float>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

} // namespace

GradientModel BackgroundGradient::fit_planar(const Image& img, int channel,
                                             const FitRegion* region) {
    GradientModel model;
    if (img.empty() || channel < 0 || channel >= img.n_channels()) return model;

    const int w = img.width(), h = img.height();
    int x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
    if (region) {
        x0 = std::clamp(region->x0, 0, w - 1); x1 = std::clamp(region->x1, 0, w - 1);
        y0 = std::clamp(region->y0, 0, h - 1); y1 = std::clamp(region->y1, 0, h - 1);
        if (x1 < x0 || y1 < y0) return model;
    }
    if (x1 - x0 + 1 < 4 * SAMPLE_STEP || y1 - y0 + 1 < 4 * SAMPLE_STEP) return model;

    // Coordinates stay in FRAME units even when sampling is restricted, so the
    // model is the same edge-to-edge tilt subtract() applies to the whole image.
    struct Sample { double x, y, z; };
    std::vector<Sample> pts;
    pts.reserve(static_cast<std::size_t>(((x1 - x0) / SAMPLE_STEP + 1)) * ((y1 - y0) / SAMPLE_STEP + 1));
    for (int y = y0; y <= y1; y += SAMPLE_STEP)
        for (int x = x0; x <= x1; x += SAMPLE_STEP) {
            const float v = img.at(x, y, channel);
            if (!std::isfinite(v)) continue;
            // A zero is an uncovered voxel, not a dark measurement.
            if (v == 0.0f) continue;
            pts.push_back({static_cast<double>(x) / w - 0.5,
                           static_cast<double>(y) / h - 0.5,
                           static_cast<double>(v)});
        }
    if (pts.size() < 64) return model;

    // Seed from the darkest samples.
    std::vector<char> keep(pts.size(), 0);
    {
        std::vector<float> zs;
        zs.reserve(pts.size());
        for (const auto& p : pts) zs.push_back(static_cast<float>(p.z));
        const float q = std::max(SEED_QUANTILE,
                                 static_cast<float>(SEED_MIN) / static_cast<float>(pts.size()));
        const float cut = quantile_of(zs, std::min(q, 1.0f));
        for (std::size_t i = 0; i < pts.size(); i++)
            keep[i] = (static_cast<float>(pts[i].z) <= cut) ? 1 : 0;
    }

    double c0 = 0.0, cx = 0.0, cy = 0.0;
    bool solved = false;
    std::vector<float> resid(pts.size());
    std::vector<float> kept_resid;
    std::vector<char>  next(pts.size());

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        // Normal equations for z = c0 + cx*x + cy*y over the kept samples.
        double n = 0, sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
        double sz = 0, sxz = 0, syz = 0;
        for (std::size_t i = 0; i < pts.size(); i++) {
            if (!keep[i]) continue;
            const double x = pts[i].x, y = pts[i].y, z = pts[i].z;
            n += 1; sx += x; sy += y;
            sxx += x * x; syy += y * y; sxy += x * y;
            sz += z; sxz += x * z; syz += y * z;
        }
        if (n < 32) break;

        // 3x3 solve by Cramer's rule; the design is well conditioned because
        // the samples span the frame.
        const double m[3][3] = {{n, sx, sy}, {sx, sxx, sxy}, {sy, sxy, syy}};
        const double det =
            m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
          - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
          + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (std::fabs(det) < 1e-18) break;   // collinear samples: no plane

        const double b[3] = {sz, sxz, syz};
        auto solve = [&](int col) {
            double a[3][3];
            for (int r = 0; r < 3; r++)
                for (int c = 0; c < 3; c++) a[r][c] = (c == col) ? b[r] : m[r][c];
            return (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
                  - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                  + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])) / det;
        };
        c0 = solve(0); cx = solve(1); cy = solve(2);
        solved = true;

        // Residuals for every sample; scatter from the KEPT ones only.
        kept_resid.clear();
        for (std::size_t i = 0; i < pts.size(); i++) {
            resid[i] = static_cast<float>(pts[i].z - (c0 + cx * pts[i].x + cy * pts[i].y));
            if (keep[i]) kept_resid.push_back(resid[i]);
        }
        std::vector<float> tmp = kept_resid;
        const float med = median_of(tmp);
        for (auto& r : tmp) r = std::fabs(r - med);
        const float sigma = 1.4826f * median_of(tmp);
        if (!(sigma > 0.0f)) break;

        std::size_t kept = 0;
        bool changed = false;
        for (std::size_t i = 0; i < pts.size(); i++) {
            next[i] = (resid[i] < med + CLIP_HIGH_SIGMA * sigma &&
                       resid[i] > med - CLIP_LOW_SIGMA * sigma) ? 1 : 0;
            kept += next[i];
            if (next[i] != keep[i]) changed = true;
        }
        if (!changed) break;                    // converged
        if (kept < pts.size() / 10) break;      // would collapse: keep this plane
        keep.swap(next);
    }
    if (!solved) return model;

    model.level = c0;
    model.dx    = cx;      // x spans -0.5..+0.5, so cx IS the edge-to-edge change
    model.dy    = cy;
    model.valid = true;
    return model;
}

void BackgroundGradient::subtract(Image& img, int channel, const GradientModel& model) {
    if (!model.valid || img.empty()) return;
    if (channel < 0 || channel >= img.n_channels()) return;

    const int w = img.width(), h = img.height();
    for (int y = 0; y < h; y++) {
        const double fy = static_cast<double>(y) / h - 0.5;
        for (int x = 0; x < w; x++) {
            float& v = img.at(x, y, channel);
            if (v == 0.0f) continue;          // uncovered stays uncovered
            const double fx = static_cast<double>(x) / w - 0.5;
            // The TILT only: `level` is deliberately left in place.
            v = static_cast<float>(v - (model.dx * fx + model.dy * fy));
        }
    }
}

double BackgroundGradient::amplitude(const GradientModel& model) {
    if (!model.valid) return 0.0;
    // The plane's extreme values sit at opposite corners of the frame.
    return std::fabs(model.dx) + std::fabs(model.dy);
}

} // namespace nukex
