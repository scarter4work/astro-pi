// AutoContrast core (PCL-free): tonal, chroma, saturation and background metrics — fingerprint/metrics.py.
#pragma once

#include "core/Image.h"
#include "core/fingerprint/Starlet.h"

#include <array>
#include <vector>

namespace ac {

// QUANTILE_LEVELS (percent), shared by the tonal (L*) and saturation (C*) vectors.
extern const std::array<double, 10> kQuantileLevels;

// np.percentile(plane, QUANTILE_LEVELS) / np.percentile(chroma, ...).
std::array<double, 10> tonal_quantiles(const Plane& L);
std::array<double, 10> saturation_quantiles(const Plane& C);

// Chroma-weighted histogram2d over [-extent, extent]^2 (weight = hypot(a, b)), normalised to sum 1 with
// np.sum's pairwise order (all-zero if the total is not > 0). Row-major bins x bins, [ia * bins + ib].
std::vector<double> chroma_histogram(const Plane& a, const Plane& b, int bins = 16, double extent = 100.0);

// p1 / p99.9 of L*; 0.0 when the peak is exactly 0.
double floor_to_peak_ratio(const Plane& L);

// sum(planes**2) over all scales / sum((residual - residual.mean())**2), each by np.sum's pairwise order;
// +inf when the gradient energy is 0 and the structure energy is > 0, 0.0 when both are 0.
double structure_to_gradient_ratio_from_transform(const Starlet& st);

// Per-channel mean of the darkest `percentile` fraction of pixels (luminance = rgb.mean(axis=-1)),
// divided by the mean of those channel means; all ones when that mean is 0. numpy's reduction order.
std::vector<double> background_channel_balance(const Image& rgb, double percentile = 25.0);

} // namespace ac
