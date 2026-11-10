// Distance -> depth strength, as the Python core's transform.py (PCL-free, reentrant).
#pragma once

#include <vector>

namespace gdg {

/// transform.depth_strength: +1 near .. -1 far from the p_low/p_high percentiles of log10(r)
/// over the finite, positive distances; `neutral` for the others, and for every row when the
/// spread is degenerate (hi <= lo).
std::vector<double> depth_strength(const std::vector<double>& r_med, double p_low = 5.0, double p_high = 95.0,
                                   double neutral = 0.0);

/// transform.confidence: 1 - clip((r_hi - r_lo) / (2 r_med), 0, 1); 0 where that is not finite.
/// Throws std::invalid_argument on a length mismatch.
std::vector<double> confidence(const std::vector<double>& r_med, const std::vector<double>& r_lo,
                               const std::vector<double>& r_hi);

/// transform.effective_strength: depth_strength * confidence, `neutral` where r_med is not
/// finite. Throws std::invalid_argument on a length mismatch.
std::vector<double> effective_strength(const std::vector<double>& r_med, const std::vector<double>& r_lo,
                                       const std::vector<double>& r_hi, double p_low = 5.0, double p_high = 95.0,
                                       double neutral = 0.0);

}  // namespace gdg
