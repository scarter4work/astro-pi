#include "nukex/fitting/model_selector.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include "nukex/fitting/student_t_fitter.hpp"
#include "nukex/fitting/gmm_fitter.hpp"
#include "nukex/fitting/contamination_fitter.hpp"
#include "nukex/fitting/kde_fitter.hpp"

#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

ModelSelector::ModelSelector() : config_(Config{}) {}

ModelSelector::ModelSelector(const Config& config) : config_(config) {}

FitResult ModelSelector::select_best(const float* values, const float* weights, int n) {
    if (n < config_.min_samples_for_fit) {
        KDEFitter kde;
        float rl = biweight_location(values, n);
        float rs = mad(values, n) * 1.4826f;
        return kde.fit(values, weights, n, rl, rs);
    }

    float rl = biweight_location(values, n);
    float rs = mad(values, n) * 1.4826f;
    if (rs < 1e-30f) rs = 1e-10f;

    // Run all parametric backends
    StudentTFitter student_t;
    FitResult result_t = student_t.fit(values, weights, n, rl, rs);

    FitResult result_gmm;
    result_gmm.converged = false;
    if (n >= config_.min_samples_for_gmm) {
        GaussianMixtureFitter gmm;
        result_gmm = gmm.fit(values, weights, n, rl, rs);
    }

    ContaminationFitter contam;
    FitResult result_c = contam.fit(values, weights, n, rl, rs);

    // Collect converged results sorted by AICc.
    // SPIKE_OUTLIER is excluded: it is a degenerate GMM (one component holds
    // > 95% of the mixture weight) that uses 5 parameters to describe what is
    // essentially a single Gaussian.  Admitting it to the AICc race would let
    // the optimizer win by overfitting — the marginal likelihood gain from the
    // tiny spike does not justify 2 extra parameters for a meaningful shape
    // classification.  The student-t or contamination fitter is a better
    // description in all such cases.
    struct Candidate {
        FitResult* result;
        double aicc;
    };
    std::vector<Candidate> candidates;
    if (result_t.converged)   candidates.push_back({&result_t,   result_t.aicc()});
    if (result_gmm.converged &&
        result_gmm.distribution.shape != DistributionShape::SPIKE_OUTLIER)
        candidates.push_back({&result_gmm, result_gmm.aicc()});
    if (result_c.converged)   candidates.push_back({&result_c,   result_c.aicc()});

    if (candidates.empty()) {
        KDEFitter kde;
        return kde.fit(values, weights, n, rl, rs);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.aicc < b.aicc;
              });

    // Best by AICc; if ΔAICc < threshold prefer the simpler model
    // (Burnham & Anderson 2002: models within 2 AICc are indistinguishable)
    FitResult* best = candidates[0].result;
    if (candidates.size() > 1) {
        double delta = candidates[1].aicc - candidates[0].aicc;
        if (delta < config_.aicc_threshold) {
            if (candidates[1].result->n_params < candidates[0].result->n_params) {
                best = candidates[1].result;
            }
        }
    }

    return *best;
}

void ModelSelector::select(const float* values, const float* weights, int n,
                           SubcubeVoxel& voxel, int channel) {
    voxel.mad[channel]                = mad(values, n);
    voxel.biweight_midvariance[channel] = biweight_midvariance(values, n);
    voxel.iqr[channel]                = iqr(values, n);

    FitResult best = select_best(values, weights, n);

    voxel.distribution[channel] = best.distribution;

    if (!best.converged) {
        voxel.set_flag(VoxelFlags::FIT_FAILED);
    }
}

} // namespace nukex
