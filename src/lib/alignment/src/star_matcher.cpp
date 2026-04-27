#include "nukex/alignment/star_matcher.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <unordered_map>
#include <vector>

namespace nukex {

namespace {

inline float dist_xy(const Star& a, const Star& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    return std::sqrt(dx*dx + dy*dy);
}

// Pick indices of the top-K brightest stars (descending flux).
std::vector<int> top_k_by_flux(const StarCatalog& cat, int K) {
    int n = cat.size();
    if (n == 0) return {};
    std::vector<int> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    if (K < n) {
        std::partial_sort(idx.begin(), idx.begin() + K, idx.end(),
            [&](int a, int b) { return cat.stars[a].flux > cat.stars[b].flux; });
        idx.resize(K);
    } else {
        std::sort(idx.begin(), idx.end(),
            [&](int a, int b) { return cat.stars[a].flux > cat.stars[b].flux; });
    }
    return idx;
}

// Canonical triangle: v[0] opposite longest side, v[1] middle, v[2] shortest.
// r1 = (middle side) / (longest side),  r2 = (shortest side) / (longest side).
// Both in (0, 1]. Translation/rotation/scale invariant.
// log_c = log10(longest side) for scale-consistency filtering across frames.
struct Triangle {
    int   v[3];
    float r1;
    float r2;
    float log_c;
};

Triangle make_triangle(const StarCatalog& cat, int i0, int i1, int i2) {
    // Side opposite vertex V is the side NOT touching V.
    // So side opposite V0 = edge(V1,V2) = s12, opposite V1 = s20, opposite V2 = s01.
    float s01 = dist_xy(cat.stars[i0], cat.stars[i1]);
    float s12 = dist_xy(cat.stars[i1], cat.stars[i2]);
    float s20 = dist_xy(cat.stars[i2], cat.stars[i0]);

    struct VS { int vert; float opp_side; };
    VS a[3] = { {i0, s12}, {i1, s20}, {i2, s01} };
    // Sort descending by opposite-side length.
    std::sort(a, a + 3,
              [](const VS& x, const VS& y) { return x.opp_side > y.opp_side; });

    Triangle t;
    t.v[0]  = a[0].vert;   // opposite longest side (= c)
    t.v[1]  = a[1].vert;   // opposite middle side (= b)
    t.v[2]  = a[2].vert;   // opposite shortest side (= a)
    float c = a[0].opp_side;
    float b = a[1].opp_side;
    float aa = a[2].opp_side;
    float c_safe = (c > 1e-6f) ? c : 1e-6f;
    t.r1    = b  / c_safe;
    t.r2    = aa / c_safe;
    t.log_c = std::log10(c_safe);
    return t;
}

std::vector<Triangle> build_triangles(
    const StarCatalog& cat, const std::vector<int>& top) {
    int K = static_cast<int>(top.size());
    std::vector<Triangle> out;
    if (K < 3) return out;
    out.reserve(static_cast<size_t>(K) * (K-1) * (K-2) / 6);
    for (int i = 0; i < K; i++)
        for (int j = i+1; j < K; j++)
            for (int k = j+1; k < K; k++) {
                Triangle t = make_triangle(cat, top[i], top[j], top[k]);
                // Reject near-degenerate triangles (nearly collinear points).
                // A ratio r2 < 0.05 means shortest side < 5% of longest — the
                // three points are almost on a line, yielding unstable
                // descriptors and numerically unreliable correspondences.
                if (t.r2 < 0.05f) continue;
                out.push_back(t);
            }
    return out;
}

inline uint64_t pack_pair(int src, int ref) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(src)) << 32)
         | static_cast<uint64_t>(static_cast<uint32_t>(ref));
}

} // anonymous namespace

std::vector<std::pair<int, int>> StarMatcher::match(
    const StarCatalog& source,
    const StarCatalog& reference,
    const Config& config) {

    if (source.size() < 3 || reference.size() < 3)
        return {};

    int K = std::min({ config.max_stars, source.size(), reference.size() });
    auto src_top = top_k_by_flux(source, K);
    auto ref_top = top_k_by_flux(reference, K);

    auto src_tris = build_triangles(source, src_top);
    auto ref_tris = build_triangles(reference, ref_top);
    if (src_tris.empty() || ref_tris.empty())
        return {};

    const float tol   = config.descriptor_tolerance;
    const float tol2  = tol * tol;

    // Sort reference triangles by r1 so we can binary-search the candidate
    // window for each source triangle. K=100 yields ~161700 triangles per
    // catalog; an unindexed O(T²) scan would be ~26 GFLOPs per frame. Sorting
    // once + O(T_src · W) window scans keeps this around 10^8 ops.
    std::sort(ref_tris.begin(), ref_tris.end(),
              [](const Triangle& a, const Triangle& b) { return a.r1 < b.r1; });

    // Vote on (src_star, ref_star) correspondences from matched triangles.
    std::unordered_map<uint64_t, int> votes;
    for (const auto& st : src_tris) {
        // Candidate ref triangles have r1 within ±tol of st.r1.
        auto lo = std::lower_bound(ref_tris.begin(), ref_tris.end(), st.r1 - tol,
            [](const Triangle& t, float v) { return t.r1 < v; });
        auto hi = std::upper_bound(ref_tris.begin(), ref_tris.end(), st.r1 + tol,
            [](float v, const Triangle& t) { return v < t.r1; });

        float best_d2 = tol2;
        const Triangle* best = nullptr;
        for (auto it = lo; it != hi; ++it) {
            if (std::abs(st.log_c - it->log_c) > config.scale_tolerance_log)
                continue;
            float dR = st.r1 - it->r1;
            float dC = st.r2 - it->r2;
            float d2 = dR*dR + dC*dC;
            if (d2 < best_d2) {
                best_d2 = d2;
                best = &*it;
            }
        }
        if (!best) continue;
        // Canonical ordering guarantees vertex-by-vertex correspondence.
        votes[pack_pair(st.v[0], best->v[0])]++;
        votes[pack_pair(st.v[1], best->v[1])]++;
        votes[pack_pair(st.v[2], best->v[2])]++;
    }

    // Extract winning correspondence per source star (highest votes ≥ min_votes).
    std::unordered_map<int, std::pair<int, int>> best_ref_for_src; // src -> {ref, count}
    for (const auto& [key, count] : votes) {
        if (count < config.min_votes) continue;
        int s = static_cast<int>(key >> 32);
        int r = static_cast<int>(key & 0xFFFFFFFFULL);
        auto it = best_ref_for_src.find(s);
        if (it == best_ref_for_src.end() || it->second.second < count) {
            best_ref_for_src[s] = { r, count };
        }
    }

    // Enforce one-to-one (a ref star can be matched by at most one src star:
    // the one with the highest vote count).
    std::unordered_map<int, std::pair<int, int>> best_src_for_ref; // ref -> {src, count}
    for (const auto& [s, rv] : best_ref_for_src) {
        int r = rv.first;
        int v = rv.second;
        auto it = best_src_for_ref.find(r);
        if (it == best_src_for_ref.end() || it->second.second < v) {
            best_src_for_ref[r] = { s, v };
        }
    }

    std::vector<std::pair<int, int>> matches;
    matches.reserve(best_src_for_ref.size());
    for (const auto& [r, sv] : best_src_for_ref)
        matches.emplace_back(sv.first, r);
    return matches;
}

std::vector<std::pair<int, int>> StarMatcher::match(
    const StarCatalog& source,
    const StarCatalog& reference,
    float /*legacy_max_distance*/) {
    return match(source, reference, Config{});
}

} // namespace nukex
