#include "Cavity.h"

#include "Channels.h"
#include "core/Numerics.h"
#include "core/Parallel.h"

#include <cmath>
#include <stdexcept>

namespace gdg::recon {

namespace {

void require_mono(const Image& a, const char* what) {
    if (a.channels != 1) throw std::invalid_argument(std::string(what) + ": expected a (ny, nx) plane");
}

/// `array ** p` for a Python-float exponent: numpy's fast_scalar_power routes 1, -1, 0, 0.5 and 2
/// to positive / reciprocal / ones_like / sqrt / square; every other exponent goes to np.power.
double np_scalar_power(double x, double p) {
    if (p == 2.0) return x * x;
    if (p == 1.0) return x;
    if (p == 0.0) return 1.0;
    if (p == 0.5) return std::sqrt(x);
    if (p == -1.0) return 1.0 / x;
    return std::pow(x, p);
}

}  // namespace

Image geometry_prior(int ny, int nx, std::pair<double, double> apex_xy, const Mask* footprint_mask, double power) {
    if (ny < 0 || nx < 0) throw std::invalid_argument("geometry_prior: negative shape");
    const double ax = apex_xy.first, ay = apex_xy.second;
    if (footprint_mask && (footprint_mask->height != ny || footprint_mask->width != nx))
        throw std::invalid_argument("footprint_mask shape (" + std::to_string(footprint_mask->height) + ", " +
                                    std::to_string(footprint_mask->width) + ") != (" + std::to_string(ny) + ", " +
                                    std::to_string(nx) + ")");
    const Mask all(nx, ny, true);
    const Mask& mask = footprint_mask ? *footprint_mask : all;

    Image r(nx, ny);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) r.at(x, y) = std::hypot(static_cast<double>(x) - ax, static_cast<double>(y) - ay);

    // R = r[mask].max(), or r.max() when the mask is empty; 1 when R <= 0.
    const bool any = mask.any();
    double R = -INFINITY;
    for (size_t i = 0; i < r.data.size(); ++i)
        if (!any || mask.data[i]) R = std::max(R, r.data[i]);
    if (r.data.empty()) R = 0.0;  // numpy would raise on an empty max; a 0-size image has no pixels
    if (R <= 0.0) R = 1.0;

    Image z(nx, ny);
    parallel_for_if(static_cast<long long>(nx) * ny * 8, ny, [&](int y) {
        for (int x = 0; x < nx; ++x) {
            const size_t i = z.index(x, y);
            const double rn = clip(r.data[i] / R, 0.0, 1.0);
            const double v = clip(1.0 - np_scalar_power(rn, power), 0.0, 1.0);
            z.data[i] = mask.data[i] ? f32(v) : 0.0;
        }
    });
    return z;
}

Image ionization_depth(const IonizationMap& ion_map, double gamma) {
    Image z = ion_map.oiii_fraction;
    for (double& v : z.data) {
        double f = clip(nan_to_num(v), 0.0, 1.0);
        if (gamma != 1.0) f = np_scalar_power(f, gamma);
        v = f32(clip(1.0 - f, 0.0, 1.0));
    }
    return z;
}

Image emission_measure_relief(const Image& lum, double sigma_small, double sigma_large, double gain) {
    require_mono(lum, "emission_measure_relief");
    Image L = lum;
    for (double& v : L.data) v = std::max(nan_to_num(v), 0.0);
    const Image small = gaussian_filter(L, sigma_small);
    const Image large = gaussian_filter(L, sigma_large);
    Image band(L.width, L.height);
    std::vector<double> mag(band.data.size());
    for (size_t i = 0; i < band.data.size(); ++i) {
        band.data[i] = small.data[i] - large.data[i];
        mag[i] = std::fabs(band.data[i]);
    }
    const double scale = mag.empty() ? 0.0 : percentile(std::move(mag), EM_NORM_PCT);
    Image z(L.width, L.height);
    if (scale <= EM_EPS) return z;
    const double neg_gain = -gain;
    for (size_t i = 0; i < z.data.size(); ++i) z.data[i] = f32(clip(neg_gain * (band.data[i] / scale), -gain, gain));
    return z;
}

Mask footprint_from_lum(const Image& lum, double frac) {
    require_mono(lum, "footprint_from_lum");
    const std::vector<double> p = percentiles(lum.data, {FOOTPRINT_SKY_PCT, FOOTPRINT_PEAK_PCT});
    const double floor = p[0], peak = p[1];
    const double rng = std::max(peak - floor, RECON_EPS);
    const double thr = floor + frac * rng;
    Mask m(lum.width, lum.height);
    for (size_t i = 0; i < lum.data.size(); ++i) m.data[i] = lum.data[i] > thr ? 1 : 0;
    if (!m.any()) m = Mask(lum.width, lum.height, true);
    return m;
}

Image linear_emissivity(const ChannelSet&) { throw std::logic_error("not implemented"); }
std::string capture_mode(const ChannelSet&) { throw std::logic_error("not implemented"); }
CavityModel reconstruct_cavity(const ChannelSet&, const IonizationMap&, const Ionizer&, const Image&, const Image&,
                               std::optional<double>, const ReconConfig&) {
    throw std::logic_error("not implemented");
}
std::string cavity_meta_json(const CavityMeta&) { throw std::logic_error("not implemented"); }

}  // namespace gdg::recon
