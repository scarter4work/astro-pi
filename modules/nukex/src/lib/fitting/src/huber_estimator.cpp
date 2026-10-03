#include "nukex/fitting/huber_estimator.hpp"
#include "nukex/fitting/robust_stats.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace nukex {

float HuberEstimator::location(const float* x, const float* fw, int n,
                               const Config& c, float* scratch,
                               float* sigma_out, float* inlier_fraction_out) {
    if (n <= 0) { if (sigma_out) *sigma_out = 0.0f; if (inlier_fraction_out) *inlier_fraction_out = 0.0f; return 0.0f; }
    std::memcpy(scratch, x, sizeof(float) * static_cast<std::size_t>(n));
    const float med = median_inplace(scratch, n);
    for (int i = 0; i < n; ++i) scratch[i] = std::fabs(x[i] - med);
    const float madv  = median_inplace(scratch, n);
    const float sigma = madv * 1.4826f;
    if (sigma_out) *sigma_out = sigma;
    if (n < c.min_samples || !(sigma > 1e-12f)) {
        if (inlier_fraction_out) *inlier_fraction_out = 1.0f;
        return med;
    }
    const float delta = c.tuning * sigma;
    float mu = med;
    int inliers = n;
    for (int it = 0; it < c.iterations; ++it) {
        double num = 0.0, den = 0.0;
        inliers = 0;
        for (int i = 0; i < n; ++i) {
            const float r  = std::fabs(x[i] - mu);
            const float hw = (r <= delta) ? 1.0f : (delta / r);
            if (r <= delta) ++inliers;
            const double ww = static_cast<double>(hw) * static_cast<double>(fw ? fw[i] : 1.0f);
            num += ww * x[i];
            den += ww;
        }
        if (den <= 0.0) break;
        const float next = static_cast<float>(num / den);
        const bool converged = std::fabs(next - mu) <= 1e-7f * std::max(1.0f, std::fabs(mu));
        mu = next;
        if (converged) break;
    }
    if (inlier_fraction_out) *inlier_fraction_out = static_cast<float>(inliers) / static_cast<float>(n);
    return mu;
}

void HuberEstimator::estimate(const float* values, const float* weights, int n,
                              SubcubeVoxel& voxel, int channel) const {
    auto& ch = voxel.channel(channel);
    ch.mad                = mad(values, n);
    ch.biweight_midvariance = biweight_midvariance(values, n);
    ch.iqr                = iqr(values, n);

    std::vector<float> scratch(static_cast<std::size_t>(std::max(n, 1)));
    float sigma = 0.0f, inlier = 0.0f;
    const float mu = location(values, weights, n, config_, scratch.data(), &sigma, &inlier);

    ZDistribution d;
    d.shape = DistributionShape::GAUSSIAN;
    d.used_nonparametric = false;
    d.true_signal_estimate = mu;
    // Effective sample count under the frame weights: (sum w)^2 / sum w^2.
    double sw = 0.0, sw2 = 0.0;
    for (int i = 0; i < n; ++i) { const double w = weights ? weights[i] : 1.0; sw += w; sw2 += w * w; }
    const double n_eff = (sw2 > 0.0) ? (sw * sw / sw2) : static_cast<double>(n);
    d.signal_uncertainty = (n_eff > 0.0) ? static_cast<float>(sigma / std::sqrt(n_eff)) : 0.0f;
    d.confidence = inlier;              // share of samples the estimator trusted fully
    d.r_squared  = 0.0f;
    d.aicc       = 0.0f;
    ch.distribution = d;
    if (n < config_.min_samples) voxel.set_flag(VoxelFlags::FIT_FAILED);
}

} // namespace nukex
