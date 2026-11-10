#include "Extinction.h"

#include "core/Morphology.h"
#include "core/Numerics.h"
#include "core/Parallel.h"

#include <cmath>
#include <stdexcept>

namespace gdg::recon {

namespace {

void require_mono(const Image& a, const char* what) {
    if (a.channels != 1) throw std::invalid_argument(std::string(what) + ": expected a (ny, nx) plane");
}

/// clip(nan_to_num(float64(a)), 0, None).
Image cleaned(const Image& a) {
    Image out = a;
    for (double& v : out.data) v = std::max(nan_to_num(v), 0.0);
    return out;
}

/// extinction._signal_gate(a) on a cleaned plane.
Image signal_gate(const Image& a) {
    const std::vector<double> p = percentiles(a.data, {DUST_SKY_PERCENTILE, DUST_BRIGHT_PERCENTILE});
    const double sky = p[0], bright = p[1];
    const double rng = bright - sky;
    if (rng < DUST_MIN_SURROUND_FRAC * std::max(bright, 1e-12)) return Image(a.width, a.height, 1, 1.0);
    const double t = DUST_MIN_SURROUND_FRAC * rng;
    Image g = gaussian_filter(a, 1.0);
    for (double& v : g.data) v = clip((v - sky - t) / t, 0.0, 1.0);
    return g;
}

}  // namespace

Image dust_from_structure(const Image& lum, int lane_px) {
    require_mono(lum, "dust_from_structure");
    const Image a = cleaned(lum);
    const std::vector<double> p = percentiles(a.data, {DUST_SKY_PERCENTILE, DUST_BRIGHT_PERCENTILE});
    const double sky = p[0], bright = p[1];
    const double rng = std::max(bright - sky, 1e-12);
    const Image smooth = gaussian_filter(a, 1.0);
    const Image surround = grey_closing(smooth, lane_px);
    const double t = DUST_MIN_SURROUND_FRAC * rng;
    Image out(a.width, a.height);
    for (size_t i = 0; i < out.data.size(); ++i) {
        const double height = surround.data[i] - sky;
        const double opacity = (surround.data[i] - smooth.data[i]) / std::max(height, 1e-12);
        const double gate = clip((height - t) / t, 0.0, 1.0);
        out.data[i] = f32(clip(opacity * gate, 0.0, 1.0));
    }
    return out;
}

Image dust_from_balmer(const Image& ha_in, const Image& hb_in, double eps) {
    require_mono(ha_in, "dust_from_balmer ha");
    require_mono(hb_in, "dust_from_balmer hb");
    validate_shapes({&ha_in, &hb_in});
    const Image ha = cleaned(ha_in), hb = cleaned(hb_in);
    const Image gate_ha = signal_gate(ha), gate_hb = signal_gate(hb);
    Image out(ha.width, ha.height);
    const int H = ha.height, W = ha.width;
    parallel_for_if(static_cast<long long>(W) * H * 8, H, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const size_t i = ha.index(x, y);
            const double excess = std::max(ha.data[i] / (hb.data[i] + eps) / CASE_B_HA_HB - 1.0, 0.0);
            const double dust = 1.0 - std::exp(-excess / BALMER_SATURATION);
            const double gate = std::min(gate_ha.data[i], gate_hb.data[i]);
            out.data[i] = f32(clip(dust * gate, 0.0, 1.0));
        }
    });
    return out;
}

}  // namespace gdg::recon
