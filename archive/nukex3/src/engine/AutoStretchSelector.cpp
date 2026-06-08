#include "engine/AutoStretchSelector.h"
#include <algorithm>
#include <cmath>

namespace nukex {

AutoStretchSelector::DistFractions
AutoStretchSelector::computeFractions(const std::vector<uint8_t>& map) {
    DistFractions f{};
    if (map.empty()) return f;

    size_t counts[4] = {0, 0, 0, 0};
    for (uint8_t t : map) {
        if (t < 4) counts[t]++;
    }

    double total = static_cast<double>(map.size());
    f.gaussian   = counts[0] / total;
    f.poisson    = counts[1] / total;
    f.skewNormal = counts[2] / total;
    f.bimodal    = counts[3] / total;
    return f;
}

double AutoStretchSelector::computeDivergence(
    const std::vector<DistFractions>& perChannel) {
    if (perChannel.size() < 2) return 0.0;

    double maxDiff = 0.0;
    for (size_t i = 0; i < perChannel.size(); ++i) {
        for (size_t j = i + 1; j < perChannel.size(); ++j) {
            maxDiff = std::max(maxDiff, std::abs(perChannel[i].gaussian   - perChannel[j].gaussian));
            maxDiff = std::max(maxDiff, std::abs(perChannel[i].poisson    - perChannel[j].poisson));
            maxDiff = std::max(maxDiff, std::abs(perChannel[i].skewNormal - perChannel[j].skewNormal));
            maxDiff = std::max(maxDiff, std::abs(perChannel[i].bimodal    - perChannel[j].bimodal));
        }
    }
    return maxDiff;
}

bool AutoStretchSelector::hasBrightOutliers(const std::vector<ChannelStats>& stats) {
    for (const auto& s : stats) {
        if (s.mad > 0 && (s.mean - s.median) / s.mad > 3.0)
            return true;
    }
    return false;
}

StretchSelection AutoStretchSelector::Select(
    const std::vector<std::vector<uint8_t>>& distTypeMaps,
    const std::vector<ChannelStats>& perChannelStats)
{
    StretchSelection result;
    size_t nCh = distTypeMaps.size();

    // 1. Compute per-channel distribution fractions
    std::vector<DistFractions> fracs(nCh);
    for (size_t c = 0; c < nCh; ++c)
        fracs[c] = computeFractions(distTypeMaps[c]);

    // Store fractions for logging
    result.fractions.resize(nCh);
    for (size_t c = 0; c < nCh; ++c) {
        result.fractions[c].gaussian   = fracs[c].gaussian;
        result.fractions[c].poisson    = fracs[c].poisson;
        result.fractions[c].skewNormal = fracs[c].skewNormal;
        result.fractions[c].bimodal    = fracs[c].bimodal;
    }

    // 2. Compute channel divergence
    result.channelDivergence = computeDivergence(fracs);

    // 3. Find max fractions across channels
    double maxBimodal = 0, maxSkewNormal = 0, maxPoisson = 0, maxGaussian = 0;
    for (const auto& f : fracs) {
        maxBimodal    = std::max(maxBimodal,    f.bimodal);
        maxSkewNormal = std::max(maxSkewNormal, f.skewNormal);
        maxPoisson    = std::max(maxPoisson,    f.poisson);
        maxGaussian   = std::max(maxGaussian,   f.gaussian);
    }

    // 4. Decision tree — MTF is the default for clean data (matches PI's STF),
    //    specialized algorithms only when distribution patterns warrant them.
    if (result.channelDivergence > 0.15) {
        result.algorithm = StretchAlgorithm::Lumpton;
        result.reason = "High channel divergence — Lumpton preserves color ratios";
    }
    else if (maxBimodal > 0.15) {
        result.algorithm = StretchAlgorithm::ArcSinh;
        result.reason = "High bimodal fraction indicates HDR/two-population pixels";
    }
    else if (maxSkewNormal > 0.20) {
        result.algorithm = StretchAlgorithm::GHS;
        result.reason = "High skew-normal fraction indicates nebulosity";
    }
    else if (maxPoisson > 0.40 && hasBrightOutliers(perChannelStats)) {
        result.algorithm = StretchAlgorithm::Veralux;
        result.reason = "High Poisson with bright outliers — Veralux for faint + highlights";
    }
    else if (maxPoisson > 0.40) {
        result.algorithm = StretchAlgorithm::GHS;
        result.reason = "High Poisson fraction — GHS with aggressive parameters";
    }
    else if (maxGaussian > 0.50) {
        result.algorithm = StretchAlgorithm::MTF;
        result.reason = "Gaussian-dominated — MTF auto-stretch (PI STF equivalent)";
    }
    else {
        result.algorithm = StretchAlgorithm::GHS;
        result.reason = "Fallback — GHS is the most versatile algorithm";
    }

    return result;
}

} // namespace nukex
