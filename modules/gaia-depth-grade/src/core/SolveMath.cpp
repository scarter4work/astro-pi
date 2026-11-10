// Plate-solver numerics (see SolveMath.h). Ported from ImageSolver 6.6.0, Copyright (c) 2013-2026
// Andres del Pozo, Copyright (c) 2019-2026 Juan Conejero (PTeam); PCL License 2.0.
#include "SolveMath.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace gdg {

namespace {
constexpr double kDeg = 180.0 / M_PI;
constexpr double kRad = M_PI / 180.0;

bool finite(const std::optional<double>& v) { return v && std::isfinite(*v); }
}  // namespace

double resolution_from_focal(double pixel_um, double focal_mm) {
    return (focal_mm > 0) ? 0.001 * (pixel_um / focal_mm) * kDeg : 0.0;
}

Seed seed_from_metadata(std::optional<double> ra_deg, std::optional<double> dec_deg,
                        std::optional<double> focal_mm, std::optional<double> pixel_um) {
    Seed s;
    s.ra = ra_deg;
    s.dec = dec_deg;
    if (pixel_um && *pixel_um > 0) s.pixel_um = pixel_um;
    if (focal_mm && *focal_mm > 0) {
        s.focal_mm = focal_mm;
        if (s.pixel_um) s.scale_deg = resolution_from_focal(*s.pixel_um, *s.focal_mm);
    }
    return s;
}

bool seed_is_valid(const Seed& s) {
    return finite(s.ra) && finite(s.dec) && *s.ra >= 0 && *s.ra <= 360 && *s.dec >= -90 && *s.dec <= 90 &&
           finite(s.scale_deg) && *s.scale_deg > 0 && *s.scale_deg <= 1800.0 / 3600.0;
}

std::optional<double> observation_jd(std::optional<double> start_jd, std::optional<double> end_jd,
                                     std::optional<double> exposure_s) {
    if (!start_jd) return std::nullopt;
    // WCSKeywords swaps a reversed start/end; the middle is the same either way.
    const double start = *start_jd;
    std::optional<double> end = end_jd;
    if (!end && exposure_s) end = start + *exposure_s / 86400;
    return end ? start + (*end - start) / 2 : start;
}

double auto_limit_magnitude(double fov_deg) {
    const double m = 14.5 * std::pow(fov_deg, -0.179);
    return std::round(100 * std::min(20.0, std::max(7.0, m))) / 100;
}

double optimal_limit_magnitude(double m, double m_db_high, long target, const std::function<long(double)>& count) {
    double m0 = 7, m1 = m_db_high;
    for (int i = 0; i < 100; ++i) {
        const long n = count(m);
        if (n < target) {
            if (m1 - m < 0.05) break;
            m0 = m;
            m += (m1 - m) / 2;
        } else if (n > 1.05 * target) {
            if (m - m0 < 0.05) break;
            m1 = m;
            m -= (m - m0) / 2;
        } else
            break;
    }
    return m;
}

int star_matching_tolerance(double min_psf_box_side) {
    return std::max(2, static_cast<int>(std::trunc(0.75 * (min_psf_box_side - 2))));
}

int blind_binning_factor(double fwhm, int width, int height) {
    const int k = std::min({static_cast<int>(std::trunc(fwhm / 3)), 8,
                            static_cast<int>(std::trunc(std::min(width, height) / 400.0))});
    return (k >= 2) ? k : 1;
}

double median_bright_fwhm(const std::vector<double>& signal, const std::vector<double>& fwhm) {
    if (signal.empty() || signal.size() != fwhm.size()) return 0.0;
    std::vector<size_t> order(signal.size());
    std::iota(order.begin(), order.end(), size_t(0));
    // Array.prototype.sort is stable: equal signals keep their order.
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return signal[a] > signal[b]; });
    std::vector<double> f;
    for (size_t i = 0; i < order.size() && i < 200; ++i) f.push_back(fwhm[order[i]]);
    std::sort(f.begin(), f.end());
    return f[f.size() >> 1];
}

double match_score(int num_valid, double rms) { return std::round(1000.0 * num_valid / (1 + rms)) / 1000; }

RectIndex::RectIndex(std::vector<Box> objects) : obj_(std::move(objects)) {
    if (obj_.empty()) return;
    double x0 = obj_[0].x0, y0 = obj_[0].y0, x1 = obj_[0].x1, y1 = obj_[0].y1, side = 0;
    for (const Box& b : obj_) {
        x0 = std::min(x0, b.x0);
        y0 = std::min(y0, b.y0);
        x1 = std::max(x1, b.x1);
        y1 = std::max(y1, b.y1);
        side += std::max(b.x1 - b.x0, b.y1 - b.y0);
    }
    gx0_ = x0;
    gy0_ = y0;
    // About 4 objects per cell, at least the mean object size, at most 1024 cells per axis.
    const double area = std::max(1.0, (x1 - x0) * (y1 - y0));
    cell_ = std::max({std::sqrt(4 * area / obj_.size()), side / obj_.size(), (x1 - x0) / 1024, (y1 - y0) / 1024,
                      1e-9});
    nx_ = static_cast<int>(std::floor((x1 - x0) / cell_)) + 1;
    ny_ = static_cast<int>(std::floor((y1 - y0) / cell_)) + 1;
    cells_.assign(static_cast<size_t>(nx_) * ny_, {});
    for (int i = 0; i < static_cast<int>(obj_.size()); ++i) {
        const Box& b = obj_[static_cast<size_t>(i)];
        const int i0 = std::clamp(static_cast<int>(std::floor((b.x0 - gx0_) / cell_)), 0, nx_ - 1);
        const int i1 = std::clamp(static_cast<int>(std::floor((b.x1 - gx0_) / cell_)), 0, nx_ - 1);
        const int j0 = std::clamp(static_cast<int>(std::floor((b.y0 - gy0_) / cell_)), 0, ny_ - 1);
        const int j1 = std::clamp(static_cast<int>(std::floor((b.y1 - gy0_) / cell_)), 0, ny_ - 1);
        for (int j = j0; j <= j1; ++j)
            for (int k = i0; k <= i1; ++k) cells_[static_cast<size_t>(j) * nx_ + k].push_back(i);
    }
}

template <typename F>
void RectIndex::visit(const Box& q, F&& f) const {
    if (obj_.empty()) return;
    const double fx0 = std::floor((q.x0 - gx0_) / cell_), fx1 = std::floor((q.x1 - gx0_) / cell_);
    const double fy0 = std::floor((q.y0 - gy0_) / cell_), fy1 = std::floor((q.y1 - gy0_) / cell_);
    if (fx1 < 0 || fy1 < 0 || fx0 > nx_ - 1 || fy0 > ny_ - 1) return;
    // One extra cell on each side absorbs rounding at cell boundaries.
    const int i0 = std::max(0, static_cast<int>(fx0) - 1), i1 = std::min(nx_ - 1, static_cast<int>(fx1) + 1);
    const int j0 = std::max(0, static_cast<int>(fy0) - 1), j1 = std::min(ny_ - 1, static_cast<int>(fy1) + 1);
    std::vector<int> found;
    for (int j = j0; j <= j1; ++j)
        for (int k = i0; k <= i1; ++k)
            for (int i : cells_[static_cast<size_t>(j) * nx_ + k]) {
                const Box& b = obj_[static_cast<size_t>(i)];
                if (b.x1 >= q.x0 && b.x0 <= q.x1 && b.y1 >= q.y0 && b.y0 <= q.y1) found.push_back(i);
            }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    f(found);
}

std::vector<int> RectIndex::search(const Box& q) const {
    std::vector<int> r;
    visit(q, [&](std::vector<int>& found) { r.swap(found); });
    return r;
}

int RectIndex::count(const Box& q) const {
    int n = 0;
    visit(q, [&](const std::vector<int>& found) { n = static_cast<int>(found.size()); });
    return n;
}

std::vector<int> remove_conflicting_sources(const std::vector<Pt>& pos, const std::vector<Box>& rect) {
    const RectIndex tree(rect);
    std::vector<int> kept;
    for (int i = 0; i < static_cast<int>(pos.size()); ++i) {
        const Pt& p = pos[static_cast<size_t>(i)];
        if (tree.count({p.x - 1, p.y - 1, p.x + 1, p.y + 1}) == 1) kept.push_back(i);
    }
    return kept;
}

}  // namespace gdg
