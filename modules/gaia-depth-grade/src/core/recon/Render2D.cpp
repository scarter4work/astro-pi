#include "Render2D.h"

#include "core/Numerics.h"
#include "core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace gdg::recon {

namespace {

constexpr int kRowChunk = 8;

/// np.gradient of a row-major line sample at index i of n (stride `step`): central difference
/// inside, one-sided first difference at either edge; 0 when the axis has a single sample.
double gradient_at(const double* p, int i, int n, size_t step) {
    if (n < 2) return 0.0;
    if (i == 0) return p[step] - p[0];
    if (i == n - 1) return p[0] - p[-static_cast<std::ptrdiff_t>(step)];
    return (p[step] - p[-static_cast<std::ptrdiff_t>(step)]) / 2.0;
}

}  // namespace

Image render_cavity_2d(const CavityModel& model, const ReconConfig& cfg) {
    const Image& E = model.emissivity;
    if (E.channels != 3)
        throw std::invalid_argument("render_cavity_2d: emissivity must be (ny, nx, 3), got " +
                                    std::to_string(E.channels) + " channel(s)");
    if (model.z.channels != 1 || model.dust.channels != 1)
        throw std::invalid_argument("render_cavity_2d: z and dust must be (ny, nx) planes");
    // The members are public: a plane replaced after construction must not be indexed past its end.
    validate_shapes({&model.z, &model.emissivity, &model.dust});

    const int nx = model.z.width, ny = model.z.height;
    Image out(nx, ny, 3);
    if (model.z.data.empty()) return out;

    const double z_min = *std::min_element(model.z.data.begin(), model.z.data.end());

    // Limb term: gradient magnitude of the smoothed depth.
    const Image zs = gaussian_filter(model.z, RENDER_LIMB_SIGMA_PX);
    Image g(nx, ny);
    parallel_for_dynamic(ny, kRowChunk, [&](int y, int) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(nx);
        for (int x = 0; x < nx; ++x) {
            const double* p = zs.data.data() + row + static_cast<size_t>(x);
            const double gx = gradient_at(p, x, nx, 1);
            const double gy = gradient_at(p, y, ny, static_cast<size_t>(nx));
            g.data[row + static_cast<size_t>(x)] = std::sqrt(gx * gx + gy * gy);
        }
    });
    std::vector<double> positive;
    for (double v : g.data)
        if (v > 0.0) positive.push_back(v);
    const bool limb_on = !positive.empty();
    const double q = limb_on ? percentile(std::move(positive), RENDER_LIMB_NORM_PCT) : 1.0;

    const double a = cfg.render_ambient, s = cfg.render_depth_shading, gamma = cfg.render_gamma;
    parallel_for_dynamic(ny, kRowChunk, [&](int y, int) {
        for (int x = 0; x < nx; ++x) {
            const size_t i = model.z.index(x, y);
            const double z_rel = model.z.data[i] - z_min;
            const double shade = std::pow(a + (1.0 - a) * clip(1.0 - s * z_rel, 0.0, 1.0), gamma);
            const double limb = limb_on ? 1.0 + cfg.render_limb_gain * clip(g.data[i] / q, 0.0, 1.0) : 1.0;
            const double T = 1.0 - clip(cfg.render_dust_strength * model.dust.data[i], 0.0, 1.0);
            for (int c = 0; c < 3; ++c) {
                const size_t k = 3 * i + static_cast<size_t>(c);
                out.data[k] = clip(f32(E.data[k] * shade * limb * T), 0.0, 1.0);
            }
        }
    });
    return out;
}

}  // namespace gdg::recon
