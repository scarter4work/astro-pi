#include "nukex/calibration/frame_normalization.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace nukex {

namespace {

/// Lower median of a non-empty vector, by partial sort. Used rather than a
/// mean so that one fogged or blown frame cannot drag the reference and
/// rescale the whole batch to match it.
///
/// Templated so the per-pixel buffers can be float: on a 24 MP frame that is
/// the difference between a 190 MB transient and a 380 MB one, and pixel
/// values arrive as float anyway, so there is nothing to lose by it.
template <typename T>
T median_of(std::vector<T>& v) {
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

} // namespace

std::vector<NormalizationCoefficients>
solve_frame_normalization(const std::vector<FrameSky>& frames) {
    std::vector<NormalizationCoefficients> out(frames.size());

    std::vector<double> locs, scales;
    locs.reserve(frames.size());
    scales.reserve(frames.size());
    for (const auto& f : frames)
        if (f.usable && f.scale > 0.0) {
            locs.push_back(f.location);
            scales.push_back(f.scale);
        }

    // Nothing measurable: every frame keeps the identity. Normalising against
    // a reference we could not measure would be worse than not normalising.
    if (locs.empty()) return out;

    const double ref_loc   = median_of(locs);
    const double ref_scale = median_of(scales);
    if (!(ref_scale > 0.0)) return out;

    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto& f = frames[i];
        if (!f.usable || !(f.scale > 0.0)) continue;   // identity
        // v' = (v - loc_f) * (s_ref / s_f) + loc_ref
        const double a = ref_scale / f.scale;
        out[i].scale  = a;
        out[i].offset = ref_loc - f.location * a;
    }
    return out;
}


FrameSky measure_channel_sky(const float* data, int width, int height) {
    FrameSky s;
    const std::size_t n = static_cast<std::size_t>(width)
                        * static_cast<std::size_t>(height);
    if (data == nullptr || width <= 0 || height <= 0) { s.usable = false; return s; }

    // Drop non-finite samples rather than let one reach nth_element, where it
    // would poison the ordering and take the batch reference with it.
    std::vector<float> v;
    v.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        if (std::isfinite(data[i])) v.push_back(data[i]);

    if (v.empty()) { s.usable = false; return s; }

    s.location = static_cast<double>(median_of(v));
    // Release before building the difference buffer: on a 24 MP frame each of
    // these is ~95 MB, and nothing needs them at the same time.
    std::vector<float>().swap(v);

    // Scale from nearest-neighbour DIFFERENCES, over every adjacent pair.
    //
    // A difference cancels anything smooth, so cloud, moon gradient and
    // vignetting drop out and what is left is pixel-to-pixel noise -- which
    // is the only thing `scale` may be, because the solver divides by it.
    // Taking the MAD *about the median difference* is what removes the
    // gradient: a smooth ramp adds the same constant to every difference.
    // Var(a - b) = 2 Var, hence the / sqrt(2).
    //
    // Deliberately NOT restricted to background pixels. Screening pairs on
    // being below the median conditions the estimate on the very noise it is
    // trying to measure, and truncates it: on flat Gaussian noise that
    // screening reported 0.00056 for a true sigma of 0.00100, and -- worse --
    // the bias vanished when a gradient was present, so the estimator
    // disagreed with itself by 1.7x between two frames of identical noise.
    // Stars need no screening here: they are a small minority of pairs and
    // the MAD ignores minorities, which is the whole reason it is a MAD.
    std::vector<float> d;
    d.reserve(n);
    for (int y = 0; y < height; ++y) {
        const float* row = data + static_cast<std::size_t>(y) * width;
        for (int x = 0; x + 1 < width; ++x) {
            const float a = row[x], b = row[x + 1];
            if (!std::isfinite(a) || !std::isfinite(b)) continue;
            // Exact in float for adjacent sky pixels: Sterbenz's lemma covers
            // any pair within a factor of two of each other.
            d.push_back(a - b);
        }
    }

    // Too few pairs to measure noise from -- a degenerate strip of an image.
    // Report scale 0, which the solver reads as "leave this frame alone",
    // rather than inventing a spread.
    if (d.size() < 64) { s.scale = 0.0; return s; }

    const float dmed = median_of(d);
    for (auto& x : d) x = std::abs(x - dmed);
    s.scale = static_cast<double>(median_of(d)) * 1.4826 / 1.4142135623730951;
    return s;
}

NormalizationCoefficients
mix_coefficients(const NormalizationCoefficients* c, const double* w, int n) {
    NormalizationCoefficients m{0.0, 0.0};
    if (c == nullptr || w == nullptr || n <= 0) return {};

    // When every input carries the same map, the mixture IS that map, and
    // saying so exactly matters more than it looks. rec709's weights sum to
    // 0.9999999999999999 in binary floating point, so mixing three identities
    // the long way returns a scale that is not 1 -- which would move every
    // OSC stack that had nothing to correct.
    bool uniform = true;
    for (int i = 1; i < n && uniform; ++i)
        uniform = (c[i].scale == c[0].scale) && (c[i].offset == c[0].offset);
    if (uniform) return c[0];

    for (int i = 0; i < n; ++i) {
        m.scale  += w[i] * c[i].scale;
        m.offset += w[i] * c[i].offset;
    }
    return m;
}

} // namespace nukex
