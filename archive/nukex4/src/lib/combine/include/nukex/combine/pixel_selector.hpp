#pragma once

#include "nukex/core/distribution.hpp"
#include "nukex/core/frame_stats.hpp"

namespace nukex {

class PixelSelector {
public:
    void select(const ZDistribution& dist,
                const float* values, const float* weights, int n,
                const FrameStats* frame_stats, const int* frame_indices,
                float welford_var,
                float& out_value, float& out_noise, float& out_snr) const;

    static float sample_variance(float value, const FrameStats& fs,
                                 float welford_var);
};

} // namespace nukex
