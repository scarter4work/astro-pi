#pragma once

#include "engine/DistributionFitter.h"
#include <vector>
#include <string>
#include <cstdint>

namespace nukex {

// Maps to pcl::AlgorithmType values for interop with StretchLibrary
enum class StretchAlgorithm {
    MTF = 0, Histogram, GHS, ArcSinh, Log,
    Lumpton, RNC, Photometric, OTS, SAS, Veralux
};

struct ChannelStats {
    double median = 0;
    double mad = 0;
    double mean = 0;
};

struct StretchSelection {
    StretchAlgorithm algorithm;
    std::string reason;
    struct ChannelFractions {
        double gaussian, poisson, skewNormal, bimodal;
    };
    std::vector<ChannelFractions> fractions;
    double channelDivergence = 0;
};

class AutoStretchSelector {
public:
    static StretchSelection Select(
        const std::vector<std::vector<uint8_t>>& distTypeMaps,
        const std::vector<ChannelStats>& perChannelStats);

private:
    struct DistFractions {
        double gaussian = 0, poisson = 0, skewNormal = 0, bimodal = 0;
    };

    static DistFractions computeFractions(const std::vector<uint8_t>& map);
    static double computeDivergence(const std::vector<DistFractions>& perChannel);
    static bool hasBrightOutliers(const std::vector<ChannelStats>& stats);
};

} // namespace nukex
