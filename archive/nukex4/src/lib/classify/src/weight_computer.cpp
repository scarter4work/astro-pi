#include "nukex/classify/weight_computer.hpp"
#include <algorithm>

namespace nukex {

WeightComputer::WeightComputer(const WeightConfig& config) : config_(config) {}

float WeightComputer::compute(float value, const FrameStats& frame_stats,
                               float welford_mean, float welford_stddev) const {
    float w = frame_stats.frame_weight * frame_stats.psf_weight;

    if (welford_stddev > 1e-30f) {
        float sigma_score = std::fabs(value - welford_mean) / welford_stddev;
        float excess = std::max(0.0f, sigma_score - config_.sigma_threshold);
        float sigma_factor = std::exp(-0.5f * excess * excess
                                      / (config_.sigma_scale * config_.sigma_scale));
        w *= sigma_factor;
    }

    // Cloud attenuation: frame-level score precomputed by the stacker
    // using the global median of per-frame medians as reference.
    w *= frame_stats.cloud_score;

    return std::max(w, config_.weight_floor);
}

} // namespace nukex
