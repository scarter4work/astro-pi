#include "Orbit.h"

#include "Render2D.h"
#include "Validate.h"
#include "core/Display.h"
#include "core/Gif.h"
#include "core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace gdg::recon {

namespace {

constexpr double kPi = 3.14159265358979323846;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::invalid_argument("orbit: " + what);
}

void check_config(const ExportConfig& ecfg, const OrbitConfig& o) {
    require(o.frames >= 1, "frames must be >= 1 (now " + std::to_string(o.frames) + ")");
    require(std::isfinite(o.amplitude_deg) && std::fabs(o.amplitude_deg) < 90.0,
            "amplitude_deg must be finite with |amplitude| < 90");
    require(o.max_width >= 1, "max_width must be >= 1 (now " + std::to_string(o.max_width) + ")");
    require(o.delay_cs >= 0 && o.delay_cs <= 65535, "delay_cs must be in [0, 65535] (now " + std::to_string(o.delay_cs) + ")");
    require(std::isfinite(ecfg.depth_ratio) && ecfg.depth_ratio >= 0.0, "depth_ratio must be finite and >= 0");
}

/// What every frame shares: the base render and the parallax depth terms.
struct Orbit {
    Image base;
    double D = 0.0;      ///< depth_ratio * R, pixels
    double zaD = 0.0;    ///< z_apex * D
};

Orbit prepare(const CavityModel& model, const ReconConfig& cfg, const ExportConfig& ecfg, const OrbitConfig& o) {
    check_config(ecfg, o);
    const Image& z = model.z;
    const int nx = z.width, ny = z.height;
    require(z.channels == 1, "z must be a plane");
    require(nx >= 1 && ny >= 1, "the model is empty");
    const double ax = model.apex_xy.first, ay = model.apex_xy.second;
    const Image& e = model.emissivity;
    const Image& d = model.dust;
    require(e.channels == 3 && d.channels == 1, "emissivity must be 3-channel and dust a plane");
    validate_shapes({&z, &e, &d});  // planes replaced after construction: never read past an end
    require_apex_in_grid(model, "orbit");
    require_finite_model(model, "orbit");

    Orbit orb;
    orb.base = render_cavity_2d(model, cfg);
    orb.D = ecfg.depth_ratio * cavity_radius_px(model);
    // z at the apex, rounded half to even and clamped to the grid (Ruling R12).
    const int ix = static_cast<int>(std::clamp(std::nearbyint(ax), 0.0, static_cast<double>(nx - 1)));
    const int iy = static_cast<int>(std::clamp(std::nearbyint(ay), 0.0, static_cast<double>(ny - 1)));
    orb.zaD = z.at(ix, iy) * orb.D;
    return orb;
}

/// tan(theta_k), theta_k = amplitude_deg * sin(2 pi k / frames) degrees. Exactly 0 for k = 0.
double tan_theta(const OrbitConfig& o, int k) {
    const double s = std::sin(2.0 * kPi * static_cast<double>(k) / static_cast<double>(o.frames));
    return std::tan(o.amplitude_deg * s * (kPi / 180.0));
}

/// One frame: every row's pixels splatted to round(x + t (z D - z_apex D)) through a z-buffer,
/// holes filled along the row (Orbit.h). Rows in parallel; each row is independent.
Image view(const Orbit& orb, const CavityModel& model, double t) {
    const Image& z = model.z;
    const Image& base = orb.base;
    const int nx = z.width, ny = z.height;
    Image out(nx, ny, 3);
    gdg::parallel_for_dynamic(ny, std::max(1, 16384 / nx), [&](int y, int) {
        std::vector<int> src(static_cast<size_t>(nx), -1);
        std::vector<double> zbuf(static_cast<size_t>(nx), 0.0);
        for (int x = 0; x < nx; ++x) {
            const double zv = z.at(x, y);
            const double target = std::nearbyint(static_cast<double>(x) + t * (zv * orb.D - orb.zaD));
            if (!(target >= 0.0 && target <= static_cast<double>(nx - 1))) continue;  // off the row (or NaN)
            const size_t tx = static_cast<size_t>(target);
            // Smaller z (nearer) wins; on equal z the earlier, lower source x stays.
            if (src[tx] < 0 || zv < zbuf[tx]) {
                src[tx] = x;
                zbuf[tx] = zv;
            }
        }
        int prev = -1;  // last hit target
        for (int tx = 0; tx < nx; ++tx) {
            const int s = src[static_cast<size_t>(tx)];
            if (s < 0) continue;
            for (int c = 0; c < 3; ++c) out.at(tx, y, c) = base.at(s, y, c);
            if (prev < 0) {
                for (int h = 0; h < tx; ++h)  // left end: the nearest hit value
                    for (int c = 0; c < 3; ++c) out.at(h, y, c) = out.at(tx, y, c);
            } else {
                for (int h = prev + 1; h < tx; ++h) {  // interior hole: linear between prev and tx
                    const double w = static_cast<double>(h - prev) / static_cast<double>(tx - prev);
                    for (int c = 0; c < 3; ++c) {
                        const double a = out.at(prev, y, c), b = out.at(tx, y, c);
                        out.at(h, y, c) = a + (b - a) * w;
                    }
                }
            }
            prev = tx;
        }
        if (prev >= 0)
            for (int h = prev + 1; h < nx; ++h)  // right end
                for (int c = 0; c < 3; ++c) out.at(h, y, c) = out.at(prev, y, c);
        // A row nobody hit stays 0.
    });
    return out;
}

}  // namespace

std::vector<Image> orbit_views(const CavityModel& model, const ReconConfig& cfg, const ExportConfig& ecfg,
                               const OrbitConfig& ocfg) {
    const Orbit orb = prepare(model, cfg, ecfg, ocfg);
    std::vector<Image> frames;
    frames.reserve(static_cast<size_t>(ocfg.frames));
    for (int k = 0; k < ocfg.frames; ++k) frames.push_back(view(orb, model, tan_theta(ocfg, k)));
    return frames;
}

void write_orbit_gif(const std::string& path, const CavityModel& model, const ReconConfig& cfg,
                     const ExportConfig& ecfg, const OrbitConfig& ocfg) {
    check_config(ecfg, ocfg);
    const int nx = model.z.width, ny = model.z.height;
    const auto [ow, oh] = gdg::downscale_size(nx, ny, ocfg.max_width);
    require(ow <= 65535 && oh <= 65535, "a " + std::to_string(ow) + "x" + std::to_string(oh) +
                                            " GIF exceeds the format's 65535 px; lower max_width");
    const Orbit orb = prepare(model, cfg, ecfg, ocfg);
    std::vector<std::vector<std::uint8_t>> frames;
    frames.reserve(static_cast<size_t>(ocfg.frames));
    gdg::StfParams stf;
    for (int k = 0; k < ocfg.frames; ++k) {
        const Image f = view(orb, model, tan_theta(ocfg, k));
        if (k == 0) stf = gdg::stf_params(f);  // one stretch for every frame: no flicker
        frames.push_back(gdg::downscale_u8(gdg::apply_stf_u8(f, stf), nx, ny, 3, ocfg.max_width));
    }
    gdg::write_gif(path, frames, ow, oh, ocfg.delay_cs);
}

}  // namespace gdg::recon
