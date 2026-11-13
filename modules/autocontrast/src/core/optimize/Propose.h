// AutoContrast core (PCL-free): deterministic action ranking (design §6.2, §3.5) — optimize/propose.py.
#pragma once

#include "core/fingerprint/Extract.h"
#include "core/optimize/Actions.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ac {

// tools/autocontrast/src/autocontrast/optimize/propose.py:60-61
constexpr double kGapGentleMax = 0.075;
constexpr double kGapModerateMax = 0.15;

// gap >= 0.15 strong, >= 0.075 moderate, else gentle.
std::string level_for_gap(double gap);

// {"spectrum","tonal","chroma","background"} (spectrum 0 when band-limiting is unusable).
std::map<std::string, double> component_gaps(const FingerprintData& ref, const FingerprintData& target);

// Per target wavelet layer: ref_shape - target_shape inside the comparable window, else 0 (all 0 when
// band-limiting is unusable).
std::vector<double> band_deficits(const FingerprintData& ref, const FingerprintData& target);

// The top_k actions most likely to close the largest gap; nullopt = the whole ranking. One action per
// (gap, scale, kind) group, level chosen by the group's gap.
std::vector<Action> propose_actions(const FingerprintData& ref, const FingerprintData& target,
                                    const std::set<std::string>& applied_kinds, int n_scales,
                                    std::optional<int> top_k);

} // namespace ac
