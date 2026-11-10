// Port of gaia_depth_grade/match.py cross_match.
//
// Python projects every catalogue row with world_to_pixel_values, asks a cKDTree over the
// detections for the nearest one (k = 1), rejects d > tolerance and keeps, per detection, the
// catalogue row with the strictly smallest d (dict insertion semantics, catalogue order). Only
// pairs within the tolerance can survive, so a uniform grid searched within the tolerance gives
// the same pairs; distances are sqrt(dx*dx + dy*dy) as cKDTree's p = 2 metric computes them.
// On an exact distance tie between two detections the lower index wins (cKDTree's choice there
// depends on its tree layout; such ties do not occur in real data).
#include "Match.h"

#include "Numerics.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gdg {

namespace {

class DetectionGrid {
public:
    DetectionGrid(const std::vector<StarRow>& det, double tol) : det_(det) {
        min_x_ = max_x_ = det[0].x;
        min_y_ = max_y_ = det[0].y;
        for (const StarRow& s : det) {
            min_x_ = std::min(min_x_, s.x);
            max_x_ = std::max(max_x_, s.x);
            min_y_ = std::min(min_y_, s.y);
            max_y_ = std::max(max_y_, s.y);
        }
        // Cells about one tolerance wide, at most 2048 per axis.
        const double span = std::max(max_x_ - min_x_, max_y_ - min_y_);
        cell_ = std::max({tol, 1.0, span / 2048.0});
        nx_ = static_cast<int>(std::floor((max_x_ - min_x_) / cell_)) + 1;
        ny_ = static_cast<int>(std::floor((max_y_ - min_y_) / cell_)) + 1;
        start_.assign(static_cast<size_t>(nx_) * ny_ + 1, 0);
        for (const StarRow& s : det) ++start_[cell_index(s.x, s.y) + 1];
        for (size_t i = 1; i < start_.size(); ++i) start_[i] += start_[i - 1];
        items_.resize(det.size());
        std::vector<size_t> fill(start_.begin(), start_.end() - 1);
        for (size_t i = 0; i < det.size(); ++i) items_[fill[cell_index(det[i].x, det[i].y)]++] = i;
    }

    /// Nearest detection to (cx, cy) with d <= tol (lowest index on a tie); false if none.
    bool nearest(double cx, double cy, double tol, size_t& best, double& best_d) const {
        // One extra cell on each side absorbs rounding in cx +- tol.
        const int i0 = std::max(0, clampi(std::floor((cx - tol - min_x_) / cell_)) - 1);
        const int i1 = std::min(nx_ - 1, clampi(std::floor((cx + tol - min_x_) / cell_)) + 1);
        const int j0 = std::max(0, clampi(std::floor((cy - tol - min_y_) / cell_)) - 1);
        const int j1 = std::min(ny_ - 1, clampi(std::floor((cy + tol - min_y_) / cell_)) + 1);
        bool found = false;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const size_t c = static_cast<size_t>(j) * nx_ + i;
                for (size_t k = start_[c]; k < start_[c + 1]; ++k) {
                    const size_t di = items_[k];
                    const double dx = cx - det_[di].x, dy = cy - det_[di].y;
                    const double d = std::sqrt(dx * dx + dy * dy);
                    if (d > tol) continue;
                    if (!found || d < best_d || (d == best_d && di < best)) {
                        found = true;
                        best = di;
                        best_d = d;
                    }
                }
            }
        return found;
    }

private:
    static int clampi(double v) {
        if (!(v > -1e9)) return -1000000000;
        if (v > 1e9) return 1000000000;
        return static_cast<int>(v);
    }
    size_t cell_index(double x, double y) const {
        const int i = std::min(nx_ - 1, static_cast<int>(std::floor((x - min_x_) / cell_)));
        const int j = std::min(ny_ - 1, static_cast<int>(std::floor((y - min_y_) / cell_)));
        return static_cast<size_t>(j) * nx_ + i;
    }

    const std::vector<StarRow>& det_;
    double min_x_, max_x_, min_y_, max_y_, cell_;
    int nx_, ny_;
    std::vector<size_t> start_, items_;
};

}  // namespace

std::vector<StarRow> cross_match(const std::vector<StarRow>& detected, const Catalog& catalog,
                                 const SkyTransform& sky, double tol_px, MatchStats& stats) {
    if (!(tol_px >= 0.0) || !std::isfinite(tol_px))
        throw std::invalid_argument("cross_match: the match tolerance must be a finite value >= 0");
    std::vector<StarRow> out = detected;
    for (StarRow& r : out) {
        r.matched = false;
        r.r_med = r.r_lo = r.r_hi = r.bp_rp = NAN;
    }
    const size_t n = out.size();
    stats = MatchStats{static_cast<int>(n), 0, 0.0, NAN};
    if (n == 0 || catalog.empty()) return out;
    for (const StarRow& s : out)
        if (!std::isfinite(s.x) || !std::isfinite(s.y))
            throw std::invalid_argument("cross_match: detection with a non-finite position");

    const DetectionGrid grid(out, tol_px);
    std::vector<long> best_ci(n, -1);
    std::vector<double> best_d(n, 0.0);
    for (size_t ci = 0; ci < catalog.size(); ++ci) {
        double cx, cy;
        if (!sky.sky_to_pixel(catalog[ci].ra, catalog[ci].dec, cx, cy) || !std::isfinite(cx) || !std::isfinite(cy))
            continue;  // astropy gives NaN, cKDTree an infinite distance: rejected
        size_t di = 0;
        double d = 0.0;
        if (!grid.nearest(cx, cy, tol_px, di, d)) continue;
        if (best_ci[di] < 0 || d < best_d[di]) {
            best_ci[di] = static_cast<long>(ci);
            best_d[di] = d;
        }
    }

    std::vector<double> offsets;
    for (size_t di = 0; di < n; ++di) {
        if (best_ci[di] < 0) continue;
        const CatalogRow& c = catalog[static_cast<size_t>(best_ci[di])];
        StarRow& r = out[di];
        r.matched = true;
        r.r_med = c.r_med;
        r.r_lo = c.r_lo;
        r.r_hi = c.r_hi;
        r.bp_rp = c.bp_rp;
        offsets.push_back(best_d[di]);
    }
    stats.n_matched = static_cast<int>(offsets.size());
    stats.match_rate = static_cast<double>(offsets.size()) / static_cast<double>(n);
    // np.median of the kept distances (order-independent).
    stats.median_offset_px = offsets.empty() ? NAN : median(offsets);
    return out;
}

}  // namespace gdg
