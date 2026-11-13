// Cavity depth transfer functions and the confidence-weighted blister CavityModel: a port of
// recon/cavity.py (PCL-free).
//
// Depth convention (CavityModel.z): 0 = nearest the observer, 1 = farthest.
// - geometry_prior: a concave bowl, apex sightline deepest, falling to the near rim.
// - ionization_depth: [O III]-rich gas forward (small z), H-alpha/[S II] skin far (large z).
// - emission_measure_relief: signed limb-brightening relief centred on 0 (bright ridges nearer).
#pragma once

#include "Model.h"

#include <optional>
#include <string>
#include <utility>

namespace gdg::recon {

inline constexpr double GEOMETRY_POWER = 2.0;
inline constexpr double IONIZATION_GAMMA = 1.0;
inline constexpr double EM_SIGMA_SMALL_PX = 1.0;
inline constexpr double EM_SIGMA_LARGE_PX = 4.0;
inline constexpr double EM_GAIN = 0.5;
inline constexpr double EM_NORM_PCT = 99.5;
inline constexpr double EM_EPS = 1e-12;
inline constexpr double FOOTPRINT_SKY_PCT = 10.0;
inline constexpr double FOOTPRINT_PEAK_PCT = 99.5;
inline constexpr double EMISSIVITY_PEAK_PCT = 99.9;
inline constexpr double RECON_EPS = 1e-12;

/// geometry_prior((ny, nx), apex_xy, footprint_mask, power): z = clip(1 - clip(r / R, 0, 1)^power,
/// 0, 1), r = hypot(x - ax, y - ay), R = the farthest footprint pixel (all pixels when the mask is
/// null or empty; 1 when R <= 0); 0 outside the footprint; float32. A mask of another shape throws
/// std::invalid_argument.
Image geometry_prior(int ny, int nx, std::pair<double, double> apex_xy, const Mask* footprint_mask = nullptr,
                     double power = GEOMETRY_POWER);

/// ionization_depth(ion_map, gamma): z = clip(1 - f^gamma, 0, 1), f = clip(nan_to_num(oiii
/// fraction), 0, 1) (the power is skipped when gamma == 1); float32.
Image ionization_depth(const IonizationMap& ion_map, double gamma = IONIZATION_GAMMA);

/// emission_measure_relief(lum, sigma_small, sigma_large, gain): L = clip(nan_to_num(lum), 0);
/// band = G(L, small) - G(L, large); scale = the 99.5th percentile of |band| (zeros when
/// <= 1e-12); z = clip(-gain * (band / scale), -gain, gain); float32. Mono only.
Image emission_measure_relief(const Image& lum, double sigma_small = EM_SIGMA_SMALL_PX,
                              double sigma_large = EM_SIGMA_LARGE_PX, double gain = EM_GAIN);

/// cavity._footprint_from_lum(lum, frac): lum > floor + frac * max(peak - floor, 1e-12), floor /
/// peak = the 10th / 99.5th percentiles; all True when nothing passes. `lum` must be finite.
Mask footprint_from_lum(const Image& lum, double frac);

/// cavity._linear_emissivity(channels): (ny, nx, 3) = ((ha + sii) / p, 0.5 oiii / p, oiii / p),
/// clipped at 0, float32; planes are clip(nan_to_num(.), 0); sii None counts as zeros; p = the
/// 99.9th percentile over the stacked ha, oiii, sii (1 when <= 1e-12).
Image linear_emissivity(const ChannelSet& channels);

/// cavity._capture_mode(channels): "sho", "duoband", "osc", "nonlinear" or "unknown".
std::string capture_mode(const ChannelSet& channels);

/// reconstruct_cavity(channels, ionization, ionizer, lum, dust, pc_per_px, config). With
/// c = clip(confidence, 0, 1), w_geo = max(w_geometry, 0), w_ion = max(w_ionization, 0) * c:
///   z = (w_geo z_geo + w_ion z_ion) / (w_geo + w_ion)   (z_geo when the sum <= 1e-12)
///   z = z + emission_sign * w_emission * relief(gain 1)
///   z = z * (1 - clip(w_dust, 0, 1) * clip(nan_to_num(dust), 0, 1))
///   z = clip(where(footprint, z, 0), 0, 1), float32
/// footprint = footprint_from_lum(clip(nan_to_num(lum), 0), footprint_frac); apex = the ionizer's
/// (x, y). Planes of different (ny, nx) throw std::invalid_argument.
CavityModel reconstruct_cavity(const ChannelSet& channels, const IonizationMap& ionization, const Ionizer& ionizer,
                               const Image& lum, const Image& dust, std::optional<double> pc_per_px,
                               const ReconConfig& config);

/// The meta dict as JSON, keys verbatim and in the Python dict's order (floats in shortest
/// round-trip form, as Python's json.dumps writes them).
std::string cavity_meta_json(const CavityMeta& meta);

/// The strict inverse of cavity_meta_json: exactly its 14 keys, each once, in any order, with
/// JSON whitespace allowed between tokens. A missing, unknown or duplicate key, a value of the
/// wrong type (numbers are JSON numbers, never NaN / Infinity; footprint_px an integer in
/// long long range; booleans true / false; strings as json_quote writes them: \" \\ \b \f \n \r \t
/// and \u00xx for the other control bytes, no raw control byte), a number that overflows a double,
/// or anything after the closing brace throws std::invalid_argument("cavity meta: ...") naming the
/// key or the problem. Doubles parse losslessly from the shortest round-trip text (bit-identical,
/// -0.0 included). Reentrant.
CavityMeta cavity_meta_from_json(const std::string& json);

}  // namespace gdg::recon
