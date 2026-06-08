#pragma once

#include "nukex/core/frame_stats.hpp"
#include <cmath>

namespace nukex {

struct WeightConfig {
    float sigma_threshold = 3.0f;
    float sigma_scale     = 2.0f;
    float cloud_threshold = 0.85f;
    float cloud_penalty   = 0.30f;
    float weight_floor    = 0.01f;
};

class WeightComputer {
public:
    explicit WeightComputer(const WeightConfig& config = {});

    float compute(float value, const FrameStats& frame_stats,
                  float welford_mean, float welford_stddev) const;

    const WeightConfig& config() const { return config_; }

private:
    WeightConfig config_;
};

} // namespace nukex
