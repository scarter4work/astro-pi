#pragma once

#include "nukex/core/voxel.hpp"

namespace nukex {

/// A Huber M-estimator of location, as the Phase B estimator.
///
/// The alternative to the model race, measured against it on four real
/// corpora (research/phase-b-model-race, Stage 2): the race's estimate is
/// 1.05x-1.47x NOISIER than Huber's on the same voxels after per-frame
/// normalisation, and Huber costs below the measurement's noise floor where
/// the race is two thirds of the whole run. This is the same estimator that
/// spike measured, made a product path: seeded at the median, scale from
/// the MAD, tuning constant 1.345 (95% Gaussian efficiency), iterated with
/// the frame weights folded into the Huber weights.
class HuberEstimator {
public:
    struct Config {
        float tuning     = 1.345f;   ///< delta = tuning * sigma
        int   iterations = 10;
        int   min_samples = 3;       ///< below this the estimate is the median
    };

    HuberEstimator() = default;
    explicit HuberEstimator(const Config& c) : config_(c) {}

    /// Estimate one channel of one voxel and write the same record fields the
    /// model race writes: robust scales, the distribution's location and
    /// uncertainty, and a confidence.
    void estimate(const float* values, const float* weights, int n,
                  SubcubeVoxel& voxel, int channel) const;

    /// The location alone. `scratch` must hold n floats. `sigma_out` receives
    /// the MAD-based scale; `inlier_fraction_out` the share of samples inside
    /// delta at convergence.
    static float location(const float* values, const float* weights, int n,
                          const Config& c, float* scratch,
                          float* sigma_out = nullptr,
                          float* inlier_fraction_out = nullptr);

private:
    Config config_;
};

} // namespace nukex
