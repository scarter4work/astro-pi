#include "catch_amalgamated.hpp"
#include "nukex/gpu/gpu_cpu_fallback.hpp"
#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/classify/weight_computer.hpp"
#include <cmath>
#include <random>
#include <iostream>

using namespace nukex;

// ── Helper: populate synthetic voxel data ──
static void fill_synthetic(ShadowBuffers& buf, int B, int C, int N,
                            std::mt19937& rng) {
    std::normal_distribution<float> gauss(0.5f, 0.05f);

    for (int vi = 0; vi < B; vi++) {
        buf.n_frames[vi] = static_cast<uint16_t>(N);

        for (int ch = 0; ch < C; ch++) {
            // Synthetic Welford accumulators
            float sum = 0.0f, sum2 = 0.0f;
            for (int fi = 0; fi < N; fi++) {
                float val = std::clamp(gauss(rng), 0.0f, 1.0f);
                buf.pixel_values[ch * N * B + fi * B + vi] = val;
                sum += val;
            }
            float mean = sum / N;
            for (int fi = 0; fi < N; fi++) {
                float v = buf.pixel_values[ch * N * B + fi * B + vi];
                sum2 += (v - mean) * (v - mean);
            }

            buf.welford_mean[ch * B + vi] = mean;
            buf.welford_M2[ch * B + vi] = sum2;
            buf.welford_n[ch * B + vi] = N;
        }
    }
}

static std::vector<FrameStats> make_frame_stats(int N) {
    std::vector<FrameStats> fs(N);
    for (int i = 0; i < N; i++) {
        fs[i].frame_weight = 1.0f;
        fs[i].psf_weight = 1.0f;
        fs[i].cloud_score = 1.0f;
        fs[i].exposure = 300.0f;
        fs[i].gain = 1.5f;
        fs[i].read_noise = 3.0f;
        fs[i].has_noise_keywords = true;
    }
    return fs;
}

// ══════════════════════════════════════════════════════════
// Kernel 1: classify_weights
// ══════════════════════════════════════════════════════════

TEST_CASE("CPU Fallback: classify_weights produces valid output", "[gpu][fallback]") {
    int B = 50, C = 3, N = 30;
    ShadowBuffers buf;
    buf.allocate(B, C, N);

    std::mt19937 rng(42);
    fill_synthetic(buf, B, C, N, rng);

    auto fs = make_frame_stats(N);
    WeightConfig config;

    GPUCPUFallback::classify_weights(buf, fs.data(), config, B, C, N);

    // Verify all weights are in [weight_floor, 1.0]
    for (int ch = 0; ch < C; ch++) {
        for (int fi = 0; fi < N; fi++) {
            for (int vi = 0; vi < B; vi++) {
                float w = buf.pixel_weights[ch * N * B + fi * B + vi];
                REQUIRE(w >= config.weight_floor - 1e-6f);
                REQUIRE(w <= 1.0f + 1e-6f);
            }
        }
    }

    // Verify summary stats are reasonable
    for (int vi = 0; vi < B; vi++) {
        REQUIRE(buf.mean_weight_out[vi] > 0.0f);
        REQUIRE(buf.total_exposure_out[vi] > 0.0f);
        REQUIRE(buf.worst_sigma_score[vi] >= 0.0f);
    }
}

TEST_CASE("CPU Fallback: classify_weights matches WeightComputer", "[gpu][fallback]") {
    // Compare CPU fallback against the existing WeightComputer::compute()
    int B = 10, C = 1, N = 20;
    ShadowBuffers buf;
    buf.allocate(B, C, N);

    std::mt19937 rng(123);
    fill_synthetic(buf, B, C, N, rng);

    auto fs = make_frame_stats(N);
    WeightConfig config;

    GPUCPUFallback::classify_weights(buf, fs.data(), config, B, C, N);

    // Verify against WeightComputer for each voxel
    WeightComputer wc(config);
    for (int vi = 0; vi < B; vi++) {
        float w_mean = buf.welford_mean[vi];
        float w_M2 = buf.welford_M2[vi];
        uint32_t w_n = buf.welford_n[vi];
        float variance = (w_n > 1) ? std::max(0.0f, w_M2) / static_cast<float>(w_n - 1) : 0.0f;
        float stddev = std::sqrt(variance);

        for (int fi = 0; fi < N; fi++) {
            float value = buf.pixel_values[fi * B + vi];
            float expected = wc.compute(value, fs[fi], w_mean, stddev);
            float got = buf.pixel_weights[fi * B + vi];
            REQUIRE(got == Catch::Approx(expected).margin(1e-5f));
        }
    }
}

// ══════════════════════════════════════════════════════════
// Kernel 2: robust_stats
// ══════════════════════════════════════════════════════════

TEST_CASE("CPU Fallback: robust_stats produces valid output", "[gpu][fallback]") {
    int B = 50, C = 3, N = 30;
    ShadowBuffers buf;
    buf.allocate(B, C, N);

    std::mt19937 rng(42);
    fill_synthetic(buf, B, C, N, rng);

    GPUCPUFallback::robust_stats(buf, B, C, N);

    for (int ch = 0; ch < C; ch++) {
        for (int vi = 0; vi < B; vi++) {
            float m = buf.mad_out[ch * B + vi];
            float bwmv = buf.biweight_midvar_out[ch * B + vi];
            float iq = buf.iqr_out[ch * B + vi];

            REQUIRE(m >= 0.0f);
            REQUIRE(bwmv >= 0.0f);
            REQUIRE(iq >= 0.0f);

            // For Gaussian data with σ≈0.05:
            // MAD ≈ 0.674 * σ ≈ 0.034
            // IQR ≈ 1.349 * σ ≈ 0.067
            REQUIRE(m < 0.2f);    // Sanity check
            REQUIRE(iq < 0.5f);
        }
    }
}

TEST_CASE("CPU Fallback: robust_stats MAD matches reference", "[gpu][fallback]") {
    // Known data: {1, 2, 3, 4, 5} → median=3, deviations={2,1,0,1,2}, MAD=1
    int B = 1, C = 1, N = 5;
    ShadowBuffers buf;
    buf.allocate(B, C, N);

    buf.n_frames[0] = 5;
    float vals[] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    for (int fi = 0; fi < 5; fi++)
        buf.pixel_values[fi * B + 0] = vals[fi] / 10.0f;  // Normalize to [0,1]

    GPUCPUFallback::robust_stats(buf, B, C, N);

    // median = 0.3, deviations from median: 0.2, 0.1, 0, 0.1, 0.2 → MAD = 0.1
    REQUIRE(buf.mad_out[0] == Catch::Approx(0.1f).margin(1e-5f));
}

// ══════════════════════════════════════════════════════════
// Kernel 3: select_pixels
// ══════════════════════════════════════════════════════════

TEST_CASE("CPU Fallback: select_pixels produces valid output", "[gpu][fallback]") {
    int B = 50, C = 3, N = 30;
    ShadowBuffers buf;
    buf.allocate(B, C, N);

    std::mt19937 rng(42);
    fill_synthetic(buf, B, C, N, rng);

    auto fs = make_frame_stats(N);
    WeightConfig config;

    // Run kernel 1 first (select_pixels needs weights)
    GPUCPUFallback::classify_weights(buf, fs.data(), config, B, C, N);

    // Set distribution results (normally from CPU fitting)
    for (int ch = 0; ch < C; ch++)
        for (int vi = 0; vi < B; vi++)
            buf.dist_true_signal[ch * B + vi] = buf.welford_mean[ch * B + vi];

    GPUCPUFallback::select_pixels(buf, fs.data(), B, C, N);

    for (int ch = 0; ch < C; ch++) {
        for (int vi = 0; vi < B; vi++) {
            float val = buf.output_value[ch * B + vi];
            float noise = buf.noise_sigma[ch * B + vi];
            float snr = buf.snr_out[ch * B + vi];

            REQUIRE(val >= 0.0f);
            REQUIRE(val <= 1.0f);
            REQUIRE(noise >= 0.0f);
            REQUIRE(snr >= 0.0f);
            REQUIRE(snr <= 9999.0f);
        }
    }
}

// ══════════════════════════════════════════════════════════
// Kernel 4: spatial_context
// ══════════════════════════════════════════════════════════

TEST_CASE("CPU Fallback: spatial_context produces valid output", "[gpu][fallback]") {
    int W = 32, H = 32, C = 3;
    std::vector<float> stacked(W * H * C);

    // Synthetic: gradient with a bright spot
    for (int ch = 0; ch < C; ch++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float v = static_cast<float>(x + y) / (W + H);
                if (x > 12 && x < 20 && y > 12 && y < 20) v = 0.9f;
                stacked[ch * W * H + y * W + x] = v;
            }

    std::vector<float> grad(W * H), bg(W * H), rms(W * H);

    GPUCPUFallback::spatial_context(stacked.data(), W, H, C,
                                     grad.data(), bg.data(), rms.data());

    // Gradient should be high at the edges of the bright spot
    float grad_center = grad[16 * W + 16];
    float grad_edge = grad[13 * W + 16];  // Edge of bright spot
    REQUIRE(grad_edge > grad_center);

    // Background should be reasonable
    for (int i = 0; i < W * H; i++) {
        REQUIRE(bg[i] >= 0.0f);
        REQUIRE(bg[i] <= 1.0f);
        REQUIRE(rms[i] >= 0.0f);
    }
}

TEST_CASE("CPU Fallback: spatial_context Sobel on uniform image is zero", "[gpu][fallback]") {
    int W = 16, H = 16, C = 1;
    std::vector<float> stacked(W * H, 0.5f);
    std::vector<float> grad(W * H), bg(W * H), rms(W * H);

    GPUCPUFallback::spatial_context(stacked.data(), W, H, C,
                                     grad.data(), bg.data(), rms.data());

    // Uniform image should have zero gradient everywhere
    for (int y = 1; y < H - 1; y++)
        for (int x = 1; x < W - 1; x++)
            REQUIRE(grad[y * W + x] == Catch::Approx(0.0f).margin(1e-6f));
}
