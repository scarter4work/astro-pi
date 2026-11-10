// Linear channel ingest for SHO / duoband / OSC data: a port of recon/channels.py (PCL-free).
//
// All outputs are float32-valued (ny, nx) planes; nothing here stretches or renormalises data.
#pragma once

#include "Model.h"

#include <map>
#include <string>

namespace gdg::recon {

inline constexpr double CONF_SHO = 1.0;
inline constexpr double CONF_DUOBAND = 0.6;
inline constexpr double CONF_OSC = 0.25;

/// Normalised-median ceiling for "linear" (channels.py: linear ~0.003, asinh-stretched ~0.27).
inline constexpr double LINEAR_MEDIAN_FRAC_MAX = 0.10;

/// from_mono_sho(ha, oiii, sii): float32 pass-through, confidence 1.0, linear. Mono planes of
/// one (ny, nx) shape, else std::invalid_argument.
ChannelSet from_mono_sho(const Image& ha, const Image& oiii, const Image& sii);

/// from_duoband(d1_hao3, d2_s2o3): oiii = min(d1, d2), ha = max(d1 - oiii, 0),
/// sii = max(d2 - oiii, 0), all in float32 arithmetic; confidence 0.6, linear.
ChannelSet from_duoband(const Image& d1_hao3, const Image& d2_s2o3);

/// from_osc_rgb(rgb): ha = R, oiii = float32(0.5 * (G + B)) in float32 arithmetic, no sii;
/// confidence 0.25, linear. std::invalid_argument unless rgb is (ny, nx, 3).
ChannelSet from_osc_rgb(const Image& rgb);

/// looks_linear(arr): false for an obviously stretched array. lo, hi = the 0.1 / 99.9
/// percentiles; a flat array (hi - lo <= 0) is linear; else (median - lo) / (hi - lo) < 0.10.
/// Every channel of `arr` is pooled, as numpy does for any ndim.
bool looks_linear(const Image& arr);

/// extract_channels(inputs, mode). mode "sho" needs keys ha, oiii, sii; "duoband" d1, d2;
/// "osc" rgb. Unknown mode, missing keys or mismatched (ny, nx) shapes throw
/// std::invalid_argument. When any probe (each input, or each RGB channel for osc) fails
/// looks_linear, the result keeps its planes but has ratio_confidence 0.0 and is_linear false.
ChannelSet extract_channels(const std::map<std::string, Image>& inputs, const std::string& mode);

}  // namespace gdg::recon
