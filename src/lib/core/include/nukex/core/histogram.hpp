#pragma once

#include <cstdint>
#include <algorithm>

namespace nukex {

/// Fixed-bin histogram for per-pixel distribution shape detection.
///
/// 16 bins is sufficient for discriminating Gaussian, bimodal, skewed, and
/// uniform distributions with sample sizes N ≤ 1000 (reservoir K=64).
/// Range is set from observed data via initialize_range(), then values
/// are binned with clamping at edges.
///
/// Contract: Call initialize_range() before update() if data is outside [0, 1].
/// The default range [0, 1] is only valid for already-normalized pixel data.
struct PixelHistogram {
    static constexpr int N_BINS = 16;

    uint32_t bins[N_BINS] = {};
    float    range_min    = 0.0f;
    float    range_max    = 1.0f;

    void initialize_range(float observed_min, float observed_max) {
        float span = observed_max - observed_min;
        float margin = 0.05f * span;
        range_min = observed_min - margin;
        range_max = observed_max + margin;
        if (range_max - range_min < 1e-10f) {
            range_min -= 0.5f;
            range_max += 0.5f;
        }
    }

    void update(float x) {
        float span = range_max - range_min;
        int bin = static_cast<int>(
            (x - range_min) / span * static_cast<float>(N_BINS));
        bin = std::clamp(bin, 0, N_BINS - 1);
        bins[bin]++;
    }

    uint32_t total_count() const {
        uint32_t total = 0;
        for (int i = 0; i < N_BINS; i++) {
            total += bins[i];
        }
        return total;
    }

    int peak_bin() const {
        int best = 0;
        for (int i = 1; i < N_BINS; i++) {
            if (bins[i] > bins[best]) {
                best = i;
            }
        }
        return best;
    }

    float bin_center(int bin) const {
        float span = range_max - range_min;
        float bin_width = span / static_cast<float>(N_BINS);
        return range_min + (static_cast<float>(bin) + 0.5f) * bin_width;
    }

    void reset() {
        for (int i = 0; i < N_BINS; i++) {
            bins[i] = 0;
        }
    }
};

} // namespace nukex
