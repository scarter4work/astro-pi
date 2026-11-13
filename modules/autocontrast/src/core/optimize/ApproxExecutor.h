// AutoContrast core (PCL-free): ApproxExecutor — a TEST-ONLY port of optimize/executors/numpy_exec.py.
//
// These approximations do NOT match PixInsight's processes and are not meant to. They exist so beam pruning,
// guardrails and convergence can be tested without PixInsight: a green offline suite proves the LOOP is correct,
// nothing about whether the output is beautiful. Built as its own static library (ac_core_approx) that only the
// tests and ac-tool link; the module never does.
//
// Numerics follow numpy to the bit except core_hdr's np.log1p: on AVX512 hosts numpy dispatches float64 log1p to
// SVML, which differs from libm's correctly-rounded-in-practice log1p by 1 ulp on ~6.5% of inputs (measured on
// this host, 2026-11-12); the result stays within 1e-12 of numpy.
#pragma once

#include "core/optimize/Executor.h"

namespace ac {

// numpy_exec.py:_sigma_for_scale — max(scale / max(pixel_scale, 1e-9) / 2.355, 0.5).
double approx_sigma_for_scale(double scale_arcsec, double pixel_scale_arcsec);

// numpy_exec.py:_max_supportable_layer — floor(log2(min(height, width))).
int max_supportable_layer(int height, int width);

class ApproxExecutor final : public Executor {
public:
   explicit ApproxExecutor(unsigned threads = 0) : threads_(threads) {}

   // NumpyExecutor.apply: local_contrast (starlet layer gain, refusing a layer past max_supportable_layer),
   // local_equalize (Gaussian unsharp), core_hdr (log1p), tonal_reshape (S-curve), black_point (1st-percentile
   // floor), chroma (about the channel mean), background_neutralize (per-channel medians), star_split (no-op),
   // then clip to [0, 1]. Throws std::invalid_argument (reported as ValueError) with Python's message for an
   // unsupportable layer or an unknown kind. `threads` never changes a result.
   Image apply(const Image& parent, const Action& action, double pixel_scale_arcsec) override;

private:
   unsigned threads_;
};

} // namespace ac
