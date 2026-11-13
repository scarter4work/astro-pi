// The C++ synthetic HII blister: test fixture of the native recon Part 2 (Tasks 21-24), generated
// here with no Python. Test tooling, PCL-free.
//
// A face-on view of a paraboloid cavity (the bowl z = 1 - (r/R)^2 that geometry_prior assumes)
// about the apex (0.55 nx, 0.45 ny), R = 0.4 min(nx, ny), with rho = r / R:
//   [O III]  0.8 for rho <= 0.5 (the ionized interior), 0.8 ((1 - rho) / 0.5)^2 out to the edge;
//   H-alpha  0.3 on the 0.6-1.0 R rim, ramping in linearly over 0.5-0.6 R;
//   [S II]   0.5 H-alpha;
//   a near-side dust lane, rows lane_y0..lane_y1 (4 px), multiplying every channel's emission
//   by 0.2 inside the footprint (rho <= 1);
//   sky 0.002 plus Gaussian noise of sigma 0.0005 on every plane;
//   rgb = (ha + sii, 0.5 oiii, oiii).
// Planes are float32-valued (as a real float32 master), y = row (frame C1). The noise is drawn from
// std::mt19937_64(seed) through an explicit Box-Muller transform (std::normal_distribution is
// implementation-defined), ha then oiii then sii, row-major: identical planes for a seed on any
// standard library.
#pragma once

#include "core/Image.h"
#include "core/recon/Model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace gdg::test {

struct BlisterTruth {
    double apex_x, apex_y, radius_px;
    double lane_y0, lane_y1;  ///< first and last lane row, inclusive
};

struct Blister {
    gdg::Image ha, oiii, sii, rgb;
    BlisterTruth truth;
};

inline constexpr double BLISTER_SKY = 0.002;
inline constexpr double BLISTER_NOISE_SIGMA = 0.0005;
inline constexpr double BLISTER_LANE_TRANSMISSION = 0.2;

namespace detail {

/// A standard normal from two 53-bit uniforms of the engine (Box-Muller, cosine branch).
inline double blister_normal(std::mt19937_64& rng) {
    constexpr double k53 = 1.0 / 9007199254740992.0;  // 2^-53
    const double u1 = (static_cast<double>(rng() >> 11) + 1.0) * k53;  // (0, 1]
    const double u2 = static_cast<double>(rng() >> 11) * k53;          // [0, 1)
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * 3.141592653589793 * u2);
}

}  // namespace detail

inline Blister make_blister(int nx = 128, int ny = 96, std::uint64_t seed = 20261112) {
    BlisterTruth t{};
    t.apex_x = 0.55 * nx;
    t.apex_y = 0.45 * ny;
    t.radius_px = 0.4 * std::min(nx, ny);
    t.lane_y0 = std::floor(t.apex_y + 0.25 * t.radius_px);
    t.lane_y1 = t.lane_y0 + 3.0;

    Blister b{gdg::Image(nx, ny), gdg::Image(nx, ny), gdg::Image(nx, ny), gdg::Image(nx, ny, 3), t};
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            const double rho = std::hypot(x - t.apex_x, y - t.apex_y) / t.radius_px;
            double ha = 0.0, oiii = 0.0;
            if (rho <= 1.0) {
                const double edge = (1.0 - rho) / 0.5;
                oiii = rho <= 0.5 ? 0.8 : 0.8 * edge * edge;
                ha = 0.3 * gdg::recon::clip((rho - 0.5) / 0.1, 0.0, 1.0);
                if (y >= t.lane_y0 && y <= t.lane_y1) {
                    oiii *= BLISTER_LANE_TRANSMISSION;
                    ha *= BLISTER_LANE_TRANSMISSION;
                }
            }
            b.ha.at(x, y) = ha;
            b.oiii.at(x, y) = oiii;
            b.sii.at(x, y) = 0.5 * ha;
        }

    std::mt19937_64 rng(seed);
    for (gdg::Image* plane : {&b.ha, &b.oiii, &b.sii})
        for (double& v : plane->data)
            v = gdg::recon::f32(v + BLISTER_SKY + BLISTER_NOISE_SIGMA * detail::blister_normal(rng));

    for (size_t i = 0; i < b.ha.pixels(); ++i) {
        b.rgb.data[3 * i] = gdg::recon::f32(b.ha.data[i] + b.sii.data[i]);
        b.rgb.data[3 * i + 1] = 0.5 * b.oiii.data[i];
        b.rgb.data[3 * i + 2] = b.oiii.data[i];
    }
    return b;
}

}  // namespace gdg::test
