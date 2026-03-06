#include "QualityWeights.h"
#include <cmath>
#include <numeric>

namespace nukex {

std::vector<double> ComputeQualityWeights(
    const std::vector<SubMetadata>& metadata,
    const WeightConfig& config)
{
    const size_t n = metadata.size();

    // Edge case: empty input
    if (n == 0) {
        return {};
    }

    // Edge case: single sub
    if (n == 1) {
        return {1.0};
    }

    // Check if all config weights are zero — return equal weights
    double totalConfigWeight = std::abs(config.fwhmWeight)
                             + std::abs(config.eccentricityWeight)
                             + std::abs(config.skyBackgroundWeight)
                             + std::abs(config.hfrWeight)
                             + std::abs(config.altitudeWeight);
    if (totalConfigWeight == 0.0) {
        return std::vector<double>(n, 1.0 / static_cast<double>(n));
    }

    std::vector<double> scores(n, 0.0);

    for (size_t i = 0; i < n; ++i) {
        double total = 0.0;
        double usedWeight = 0.0;
        const auto& m = metadata[i];

        // FWHM — lower is better
        if (config.fwhmWeight != 0.0 && m.fwhm > 0.0) {
            total += config.fwhmWeight * (1.0 / (1.0 + m.fwhm));
            usedWeight += config.fwhmWeight;
        }

        // Eccentricity — lower is better
        if (config.eccentricityWeight != 0.0 && m.eccentricity > 0.0) {
            total += config.eccentricityWeight * (1.0 / (1.0 + m.eccentricity));
            usedWeight += config.eccentricityWeight;
        }

        // Sky background — lower is better
        if (config.skyBackgroundWeight != 0.0 && m.skyBackground > 0.0) {
            total += config.skyBackgroundWeight * (1.0 / (1.0 + m.skyBackground));
            usedWeight += config.skyBackgroundWeight;
        }

        // HFR — lower is better
        if (config.hfrWeight != 0.0 && m.hfr > 0.0) {
            total += config.hfrWeight * (1.0 / (1.0 + m.hfr));
            usedWeight += config.hfrWeight;
        }

        // Altitude — higher is better (airmass proxy)
        if (config.altitudeWeight != 0.0 && m.altitude > 0.0) {
            total += config.altitudeWeight * std::sin(m.altitude * M_PI / 180.0);
            usedWeight += config.altitudeWeight;
        }

        // Normalize by the sum of weights actually used for this sub,
        // so that missing metrics don't penalize a sub
        if (usedWeight > 0.0) {
            scores[i] = total / usedWeight;
        } else {
            // No usable metrics — assign neutral score
            scores[i] = 1.0;
        }
    }

    // Normalize scores to sum to 1.0
    double sum = std::accumulate(scores.begin(), scores.end(), 0.0);

    std::vector<double> weights(n);
    if (sum > 0.0) {
        for (size_t i = 0; i < n; ++i) {
            weights[i] = scores[i] / sum;
        }
    } else {
        // Fallback: equal weights
        for (size_t i = 0; i < n; ++i) {
            weights[i] = 1.0 / static_cast<double>(n);
        }
    }

    return weights;
}

} // namespace nukex
