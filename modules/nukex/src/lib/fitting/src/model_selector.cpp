#include "nukex/fitting/model_selector.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include "nukex/fitting/student_t_fitter.hpp"
#include "nukex/fitting/gmm_fitter.hpp"
#include "nukex/fitting/contamination_fitter.hpp"
#include "nukex/fitting/kde_fitter.hpp"

#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>

namespace nukex {

// ══════════════════════════════════════════════════════════════════════
// RESEARCH INSTRUMENTATION — do not ship.
//
// Env-gated per-voxel dump of the model race, for the 2026-09-07 spike
// asking whether a Huber M-estimator + gap classifier can replace the
// three-optimiser AICc race. Costs one relaxed atomic load per call when
// NUKEX_DUMP_VOXELS is unset.
//
//   NUKEX_DUMP_VOXELS=<path>    enable, write records to <path>
//   NUKEX_DUMP_STRIDE=<pow2>    keep ~1/stride of voxel-channels (default 512)
//   NUKEX_DUMP_MAX=<n>          stop after n records (default 200000)
//
// Sampling is a hash of the sample vector's own bits, so it is
// deterministic, reproducible and independent of thread scheduling.
// ══════════════════════════════════════════════════════════════════════
namespace {

struct VoxelDump {
    std::FILE*  fp     = nullptr;
    uint64_t    mask   = 511;
    uint64_t    max_n  = 200000;
    uint64_t    count  = 0;
    std::mutex  mtx;
};

VoxelDump* voxel_dump() {
    static VoxelDump* d = [] () -> VoxelDump* {
        const char* path = std::getenv("NUKEX_DUMP_VOXELS");
        if (!path || !*path) return nullptr;
        auto* v = new VoxelDump();
        v->fp = std::fopen(path, "wb");
        if (!v->fp) { delete v; return nullptr; }
        if (const char* s = std::getenv("NUKEX_DUMP_STRIDE")) {
            uint64_t st = std::strtoull(s, nullptr, 10);
            if (st >= 1 && (st & (st - 1)) == 0) v->mask = st - 1;
        }
        if (const char* s = std::getenv("NUKEX_DUMP_MAX")) {
            uint64_t m = std::strtoull(s, nullptr, 10);
            if (m > 0) v->max_n = m;
        }
        // Header: magic, version, stride
        uint32_t hdr[3] = { 0x4456584Eu /* "NXVD" */, 2u,
                            static_cast<uint32_t>(v->mask + 1) };
        std::fwrite(hdr, sizeof(uint32_t), 3, v->fp);
        return v;
    }();
    return d;
}

inline uint64_t sample_hash(const float* values, int n) {
    uint64_t h = 1469598103934665603ull;          // FNV-1a offset basis
    const unsigned char* p = reinterpret_cast<const unsigned char*>(values);
    const size_t nb = static_cast<size_t>(n) * sizeof(float);
    for (size_t i = 0; i < nb; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    h ^= static_cast<uint64_t>(n); h *= 1099511628211ull;
    // Final avalanche (splitmix64 finaliser) so the low bits are usable.
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27; h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
}

void dump_record(VoxelDump* d, const float* values, const float* weights, int n,
                 int channel, double rl, double rs, double mad_raw,
                 const FitResult& t, const FitResult& g, const FitResult& c,
                 const FitResult& best, int winner_model) {
    double f[36] = {0.0};
    f[0]  = rl;
    f[1]  = rs;
    f[2]  = n;
    f[3]  = t.converged ? 1.0 : 0.0;
    f[4]  = t.distribution.params.student_t.mu;
    f[5]  = t.distribution.params.student_t.sigma;
    f[6]  = t.distribution.params.student_t.nu;
    f[7]  = t.log_likelihood;
    f[8]  = t.converged ? t.aicc() : 0.0;
    f[9]  = g.converged ? 1.0 : 0.0;
    f[10] = g.distribution.params.bimodal.comp1.mu;
    f[11] = g.distribution.params.bimodal.comp1.sigma;
    f[12] = g.distribution.params.bimodal.mixing_ratio;
    f[13] = g.distribution.params.bimodal.comp2.mu;
    f[14] = g.distribution.params.bimodal.comp2.sigma;
    f[15] = g.distribution.true_signal_estimate;
    f[16] = static_cast<double>(static_cast<int>(g.distribution.shape));
    f[17] = g.log_likelihood;
    f[18] = g.converged ? g.aicc() : 0.0;
    f[19] = c.converged ? 1.0 : 0.0;
    f[20] = c.distribution.params.contamination.mu;
    f[21] = c.distribution.params.contamination.sigma;
    f[22] = c.distribution.params.contamination.contamination_frac;
    f[23] = c.log_likelihood;
    f[24] = c.converged ? c.aicc() : 0.0;
    f[25] = winner_model;
    f[26] = static_cast<double>(static_cast<int>(best.distribution.shape));
    f[27] = best.distribution.true_signal_estimate;
    f[28] = best.distribution.signal_uncertainty;
    f[29] = best.distribution.confidence;
    f[30] = best.converged ? 1.0 : 0.0;
    f[31] = mad_raw;
    f[32] = channel;
    f[33] = best.n_params;
    f[34] = best.log_likelihood;
    f[35] = best.converged ? best.aicc() : 0.0;

    std::lock_guard<std::mutex> lock(d->mtx);
    if (d->count >= d->max_n) return;
    uint32_t nn = static_cast<uint32_t>(n);
    std::fwrite(&nn, sizeof(uint32_t), 1, d->fp);
    std::fwrite(values,  sizeof(float), n, d->fp);
    std::fwrite(weights, sizeof(float), n, d->fp);
    std::fwrite(f, sizeof(double), 36, d->fp);
    std::fflush(d->fp);   // PI may exit without flushing stdio
    ++d->count;
}

} // namespace

ModelSelector::ModelSelector() : config_(Config{}) {}

ModelSelector::ModelSelector(const Config& config) : config_(config) {}

FitResult ModelSelector::select_best(const float* values, const float* weights, int n,
                                     int dump_channel) {
    if (n < config_.min_samples_for_fit) {
        KDEFitter kde;
        float rl = biweight_location(values, n);
        float rs = mad(values, n) * 1.4826f;
        FitResult r = kde.fit(values, weights, n, rl, rs);
        if (VoxelDump* d = voxel_dump()) {
            if (n > 0 && (sample_hash(values, n) & d->mask) == 0) {
                FitResult none;
                dump_record(d, values, weights, n, dump_channel, rl, rs,
                            mad(values, n), none, none, none, r, 3);
            }
        }
        return r;
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
        FitResult r = kde.fit(values, weights, n, rl, rs);
        if (VoxelDump* d = voxel_dump()) {
            if ((sample_hash(values, n) & d->mask) == 0) {
                dump_record(d, values, weights, n, dump_channel, rl, rs,
                            mad(values, n), result_t, result_gmm, result_c, r, 3);
            }
        }
        return r;
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

    if (VoxelDump* d = voxel_dump()) {
        if ((sample_hash(values, n) & d->mask) == 0) {
            int winner = (best == &result_t)   ? 0
                       : (best == &result_gmm) ? 1
                       : (best == &result_c)   ? 2 : 3;
            dump_record(d, values, weights, n, dump_channel, rl, rs,
                        mad(values, n), result_t, result_gmm, result_c,
                        *best, winner);
        }
    }

    return *best;
}

void ModelSelector::select(const float* values, const float* weights, int n,
                           SubcubeVoxel& voxel, int channel) {
    voxel.channel(channel).mad                = mad(values, n);
    voxel.channel(channel).biweight_midvariance = biweight_midvariance(values, n);
    voxel.channel(channel).iqr                = iqr(values, n);

    FitResult best = select_best(values, weights, n, channel);

    voxel.channel(channel).distribution = best.distribution;

    if (!best.converged) {
        voxel.set_flag(VoxelFlags::FIT_FAILED);
    }
}

} // namespace nukex
