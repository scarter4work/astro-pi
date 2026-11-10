#include "HaloProfile.h"

#include "Numerics.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

namespace gdg {

namespace {

// Reference stars must peak below this to be unsaturated (psf._SATURATION_CEILING).
constexpr double kSaturationCeiling = 0.9;
const double kNaN = std::numeric_limits<double>::quiet_NaN();

// np.linspace(start, stop, num): i*step + start, the last sample exactly stop.
std::vector<double> linspace(double start, double stop, long num) {
    std::vector<double> y(static_cast<size_t>(std::max(num, 0L)));
    if (num <= 0) return y;
    const long div = num - 1;
    const double delta = stop - start;
    if (div > 0) {
        const double step = delta / static_cast<double>(div);
        for (long i = 0; i < num; ++i)
            y[static_cast<size_t>(i)] = (step == 0.0 ? (static_cast<double>(i) / static_cast<double>(div)) * delta
                                                     : static_cast<double>(i) * step) + start;
        y.back() = stop;
    } else {
        y[0] = 0.0 * delta + start;
    }
    return y;
}

// ndarray.max(): NaN if any element is NaN.
double np_max(const std::vector<double>& v) {
    double m = -std::numeric_limits<double>::infinity();
    for (double x : v) {
        if (std::isnan(x)) return kNaN;
        m = std::max(m, x);
    }
    return m;
}

// np.median on a non-empty selection: NaN if any element is NaN.
double np_median(std::vector<double> v) {
    for (double x : v)
        if (std::isnan(x)) return kNaN;
    return median(std::move(v));
}

// psf._radial_median(cut, cx, cy, r_grid) for a square (2 rad + 1) stamp centred at (rad, rad).
std::vector<double> radial_median(const std::vector<double>& cut, long size, long c, const std::vector<double>& r_grid) {
    if (r_grid.size() < 2) throw std::invalid_argument("build_halo_profile: radius must give at least 2 grid points");
    std::vector<double> r(cut.size());
    for (long y = 0; y < size; ++y)
        for (long x = 0; x < size; ++x)
            r[static_cast<size_t>(y * size + x)] = std::sqrt(static_cast<double>((x - c) * (x - c) + (y - c) * (y - c)));
    const double step = r_grid[1] - r_grid[0];
    const double half = step / 2.0;
    std::vector<double> out(r_grid.size(), kNaN);
    std::vector<double> sel;
    for (size_t i = 0; i < r_grid.size(); ++i) {
        sel.clear();
        for (size_t q = 0; q < r.size(); ++q)
            if (std::fabs(r[q] - r_grid[i]) <= half) sel.push_back(cut[q]);
        if (!sel.empty()) out[i] = np_median(sel);
    }
    return out;
}

std::string fallback_warning(size_t n, int need) {
    return "star-shine: only " + std::to_string(n) + " clean reference star(s) (need " + std::to_string(need) +
           ") \xE2\x80\x94 using a Moffat halo this run, not the image's own PSF";
}

}  // namespace

std::vector<double> HaloProfile::sample(const std::vector<double>& r) const {
    if (r_grid.empty() || r_grid.size() != values.size()) throw std::logic_error("HaloProfile: empty or ragged profile");
    std::vector<double> out = interp(r, r_grid, values, values[0], 0.0);
    const double last = r_grid.back();
    for (size_t i = 0; i < r.size(); ++i)
        if (r[i] > last) out[i] = 0.0;
    return out;
}

HaloProfile moffat_profile(double base_sigma_px, double beta, double radius_px) {
    const double fwhm = 2.3548 * base_sigma_px;
    const double alpha = fwhm / (2.0 * std::sqrt(std::pow(2.0, 1.0 / beta) - 1.0));
    HaloProfile h;
    h.r_grid = linspace(0.0, radius_px, std::max(static_cast<long>(radius_px) + 1, 2L));
    h.values.resize(h.r_grid.size());
    for (size_t i = 0; i < h.r_grid.size(); ++i) {
        const double t = h.r_grid[i] / alpha;
        h.values[i] = std::pow(1.0 + t * t, -beta);
    }
    const double v0 = h.values[0];
    for (double& v : h.values) v = v / v0;
    h.is_empirical = false;
    return h;
}

HaloProfile build_halo_profile(const Image& stars, const std::vector<StarRow>& table, double base_sigma_px,
                               const GradeConfig& cfg, const Log& log) {
    const Image lum = luminance(stars);
    const double radius_px = cfg.halo_radius_sigma * base_sigma_px;
    const std::vector<double> r_grid = linspace(0.0, radius_px, static_cast<long>(radius_px) + 1);
    const long rad = static_cast<long>(std::ceil(radius_px));

    // np.argsort(flux)[::-1] as a stable argsort reversed: NaN sorts last, so it comes first.
    std::vector<size_t> order(table.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const double fa = table[a].flux, fb = table[b].flux;
        if (std::isnan(fa)) return false;
        if (std::isnan(fb)) return true;
        return fa < fb;
    });
    std::reverse(order.begin(), order.end());

    const long nx = lum.width, ny = lum.height;
    const long size = 2 * rad + 1;
    const size_t stop_at = static_cast<size_t>(std::max(cfg.halo_min_refs * 3, cfg.halo_min_refs));
    const double far_r = base_sigma_px * 4.0;
    std::vector<std::vector<double>> stack;
    std::vector<double> cut(static_cast<size_t>(size * size)), far_vals;
    for (size_t i : order) {
        const long xi = static_cast<long>(std::nearbyint(table[i].x));  // Python round(): half to even
        const long yi = static_cast<long>(std::nearbyint(table[i].y));
        if (xi - rad < 0 || yi - rad < 0 || xi + rad >= nx || yi + rad >= ny) continue;  // frame cuts the stamp
        for (long y = 0; y < size; ++y)
            for (long x = 0; x < size; ++x)
                cut[static_cast<size_t>(y * size + x)] = lum.at(static_cast<int>(xi - rad + x), static_cast<int>(yi - rad + y));
        const double peak = np_max(cut);
        if (!std::isfinite(peak) || peak <= 0.0 || peak >= kSaturationCeiling) continue;  // dark or saturated
        far_vals.clear();
        for (long y = 0; y < size; ++y)
            for (long x = 0; x < size; ++x)
                if (std::sqrt(static_cast<double>((x - rad) * (x - rad) + (y - rad) * (y - rad))) > far_r)
                    far_vals.push_back(cut[static_cast<size_t>(y * size + x)]);
        if (!far_vals.empty() && np_max(far_vals) > 0.5 * peak) continue;  // a bright neighbour
        std::vector<double> prof = radial_median(cut, size, rad, r_grid);
        for (double& v : prof) v = v / peak;
        stack.push_back(std::move(prof));
        if (stack.size() >= stop_at) break;
    }

    if (stack.size() < static_cast<size_t>(std::max(cfg.halo_min_refs, 0))) {
        log_warning(log, fallback_warning(stack.size(), cfg.halo_min_refs));
        return moffat_profile(base_sigma_px, cfg.halo_moffat_beta, radius_px);
    }

    // halo_min_refs <= 0 with no stamp: numpy's vstack([]) raises, so does the port.
    if (stack.empty()) throw std::invalid_argument("build_halo_profile: no reference stamp to stack");
    std::vector<double> values = nanmedian_rows(stack);
    for (double& v : values) {  // np.nan_to_num(values, nan=0.0)
        if (std::isnan(v)) v = 0.0;
        else if (v == std::numeric_limits<double>::infinity()) v = DBL_MAX;
        else if (v == -std::numeric_limits<double>::infinity()) v = -DBL_MAX;
    }
    if (values[0] <= 0.0) values[0] = 1.0;
    const double v0 = values[0];
    for (double& v : values) v = v / v0;
    for (size_t i = 1; i < values.size(); ++i) values[i] = std::min(values[i], values[i - 1]);  // minimum.accumulate
    HaloProfile h;
    h.r_grid = r_grid;
    h.values = std::move(values);
    h.is_empirical = true;
    return h;
}

}  // namespace gdg
