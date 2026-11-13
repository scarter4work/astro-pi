// AutoContrast core (PCL-free): the fingerprint distance function (design §4.5) — fingerprint/distance.py.
//
//    D = w_e · D_spectrum + w_t · D_tonal + w_c · D_chroma + w_b · D_background, divided by the used weight.
//
// The chroma term is dropped (and its weight redistributed) when the two palettes are not chroma-compatible
// (§2.3); that gate lives here in code, never in the user-editable weights.
#pragma once

#include "core/fingerprint/Extract.h"

#include <utility>

namespace ac {

struct DistanceWeights {
   double spectrum = 0.40, tonal = 0.25, chroma = 0.20, background = 0.15;
};

struct DistanceComponents {
   double spectrum, tonal, chroma, background;
};

// Option (b): chroma gated out on a palette mismatch and its weight redistributed over the survivors.
// Throws std::invalid_argument, naming the weights, when the surviving weights sum to zero (Python raises
// ZeroDivisionError there; neither side returns a number).
double combine_distance(const DistanceComponents& c, const DistanceWeights& w, bool palette_match);

// Band-limited spectral shape distance on [0, 1]: (distance, usable). Unusable (no comparable band) -> (0, false).
std::pair<double, bool> spectrum_distance(const FingerprintData& ref, const FingerprintData& target);

// RMS difference of the L* quantiles over the L* range (100).
double tonal_distance(const FingerprintData& ref, const FingerprintData& target);

// Exact EMD between the a*/b* chroma histograms on the bin-centre grid of [-extent, extent]^2, divided by the
// grid diagonal. 0 when either histogram is empty (a neutral image). Throws std::invalid_argument when the two
// histograms are not the same square size.
double chroma_distance(const FingerprintData& ref, const FingerprintData& target, double extent = 100.0);

// x / (1 + x) for finite x, 1 otherwise (inf and NaN alike, as np.isfinite).
double squash(double x);

// Mean of the floor/peak, structure/gradient and channel-balance sub-distances.
double background_distance(const FingerprintData& ref, const FingerprintData& target);

// The full D: palette gate, spectrum dropped (weight zeroed) when band-limiting is unusable.
double fingerprint_distance(const FingerprintData& ref, const FingerprintData& target,
                            const DistanceWeights& weights = {});

} // namespace ac
