#pragma once

#include <cstdint>
#include <algorithm>

namespace nukex {

/// Reservoir sampling (Vitter 1985) with self-describing samples.
///
/// Retains K representative samples from a stream of N items. Each item in the
/// stream has equal probability K/N of being in the final reservoir. No bias
/// toward early or late items.
///
/// Each retained sample carries its own frame context (gain, read noise, PSF
/// weight, etc.) so that pixel selection can proceed without frame-level lookups.
///
/// Reference: Vitter J.S. (1985), "Random Sampling with a Reservoir."
/// ACM Transactions on Mathematical Software 11(1):37-57.
struct ReservoirSample {
    static constexpr int K = 64;

    struct Sample {
        float    value              = 0.0f;
        float    frame_weight       = 1.0f;
        float    psf_weight         = 1.0f;
        float    read_noise         = 3.0f;    // electrons
        float    gain               = 1.0f;    // e-/ADU
        float    exposure           = 0.0f;    // seconds
        float    lum_ratio          = 1.0f;    // pixel_value / frame_median
        float    sigma_score        = 0.0f;    // deviation from Z-median in σ
        uint16_t frame_index        = 0;       // arrival order
        bool     is_meridian_flipped = false;
    };

    Sample   samples[K] = {};
    int64_t  count       = 0;

    void seed(uint64_t s) {
        rng_state = s;
        if (rng_state == 0) rng_state = 1;
    }

    void update(const Sample& s) {
        if (count < K) {
            samples[static_cast<int>(count)] = s;
        } else {
            // Cast to uint64_t for modulo to avoid signed overflow on count+1
            uint64_t j = next_random() % static_cast<uint64_t>(count + 1);
            if (j < static_cast<uint64_t>(K)) {
                samples[static_cast<int>(j)] = s;
            }
        }
        count++;
    }

    int stored_count() const {
        return (count < K) ? static_cast<int>(count) : K;
    }

    void reset() {
        count = 0;
        for (int i = 0; i < K; i++) {
            samples[i] = Sample{};
        }
    }

private:
    uint64_t rng_state = 1;

    uint64_t next_random() {
        rng_state ^= rng_state >> 12;
        rng_state ^= rng_state << 25;
        rng_state ^= rng_state >> 27;
        return rng_state * 0x2545F4914F6CDD1DULL;
    }
};

} // namespace nukex
