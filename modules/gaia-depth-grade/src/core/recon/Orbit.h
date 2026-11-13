// The cavity model as a small parallax orbit animation (recon Part 2, native; PCL-free).
//
// Frame k of n: theta_k = amplitude_deg * sin(2 pi k / n) degrees, a small yaw about the apex.
// From base = render_cavity_2d(model, cfg), each pixel (x, y) moves along its row to
//   x' = x + tan(theta_k) (z D - z_apex D)
// with D = depth_ratio * cavity_radius_px(model) (Glb.h; pixel units) and z_apex = z at the apex
// rounded (half to even) and clamped to the grid, so the apex stays put. It lands on round(x')
// (half to even) when that is inside the row; a target hit twice keeps the smaller z (the nearer
// surface), the lower source x on equal z (equal z means an equal shift, so only out-of-range
// arithmetic could tie). Targets nobody hit are filled by linear interpolation between the nearest
// hit targets of the row, the nearest hit value beyond the first or last; a row nobody hit is 0.
// theta = 0 (frame 0) is the base exactly.
#pragma once

#include "Glb.h"
#include "Model.h"

#include <string>
#include <vector>

namespace gdg::recon {

struct OrbitConfig {
    int frames = 36;             ///< frames per loop (the Python plan's default)
    double amplitude_deg = 12.0; ///< peak yaw in degrees (the Python plan's default); |a| < 90
    int max_width = 760;         ///< the GIF is box-downscaled to at most this width
    int delay_cs = 8;            ///< GIF frame delay, centiseconds
};

/// The orbit's frames at full resolution, (ny, nx, 3) linear like render_cavity_2d. Throws
/// std::invalid_argument for frames < 1, a non-finite or |amplitude_deg| >= 90, max_width < 1,
/// delay_cs outside [0, 65535], a non-finite or negative depth_ratio, an empty model, planes of the
/// wrong channel count, a non-finite apex or z / emissivity / dust value (the message names the
/// first such pixel). Rows run in parallel; reentrant.
/// Memory: n full-size frames; write_orbit_gif() holds one at a time.
std::vector<Image> orbit_views(const CavityModel& model, const ReconConfig& cfg, const ExportConfig& ecfg,
                               const OrbitConfig& ocfg);

/// The orbit as an animated GIF at `path` (loops forever): STF parameters from frame 0
/// (stf_params) applied to every frame (apply_stf_u8; no flicker), downscale_u8 to max_width, then
/// write_gif (atomic; "cannot write <path>: <strerror>"). Same argument checks as orbit_views,
/// all made before anything is rendered or written.
void write_orbit_gif(const std::string& path, const CavityModel& model, const ReconConfig& cfg,
                     const ExportConfig& ecfg, const OrbitConfig& ocfg);

}  // namespace gdg::recon
