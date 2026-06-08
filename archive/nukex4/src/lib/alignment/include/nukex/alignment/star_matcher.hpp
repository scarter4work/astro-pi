#pragma once

#include "nukex/alignment/types.hpp"

namespace nukex {

/// Match stars between two catalogs using triangle similarity matching.
///
/// Pure positional nearest-neighbour matching fails on real multi-hour imaging
/// sessions where cumulative tracking drift is 10s to 100s of pixels. This
/// matcher uses the Groth (1986) / Valdes (1995) algorithm: form triangles
/// from top-K brightest stars, compute translation/rotation/scale-invariant
/// descriptors `(r1, r2) = (b/c, a/c)` from sorted side lengths, match
/// triangles in descriptor space, then vote on star correspondences.
///
/// The returned correspondences are fed to HomographyComputer::compute().
class StarMatcher {
public:
    struct Config {
        // Triangle similarity matching parameters. max_stars=100 is tuned for
        // real multi-hour astronomical sessions where flux-based top-K is
        // unstable (cirrus / extinction changes which stars rank highest).
        // With K=100 we get robust overlap even when only ~50% of the top-100
        // stars are shared between frames.
        int   max_stars            = 100;     // top-K brightest to form triangles from
        float descriptor_tolerance = 0.003f;  // match (r1,r2) tolerance (descriptor space)
        float scale_tolerance_log  = 0.02f;   // |log10(c_src / c_ref)| bound (~5%)
        int   min_votes            = 4;       // per-correspondence vote threshold
        int   min_matches          = 8;       // minimum accepted match count

        // No longer used — kept so callers that set it still compile. Triangle
        // matching is position-invariant; there is no positional tolerance.
        float max_distance         = 0.0f;
    };

    /// Find star correspondences between source and reference catalogs.
    /// Returns pairs of (source_idx, ref_idx).
    static std::vector<std::pair<int, int>> match(
        const StarCatalog& source,
        const StarCatalog& reference,
        const Config& config);

    /// Convenience overload — uses default Config. The float parameter is
    /// ignored (kept for backward source compatibility with older callers that
    /// passed a positional max_distance).
    static std::vector<std::pair<int, int>> match(
        const StarCatalog& source,
        const StarCatalog& reference,
        float /*legacy_max_distance*/);
};

} // namespace nukex
