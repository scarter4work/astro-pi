#include "catch_amalgamated.hpp"
#include "nukex/gpu/gpu_cpu_fallback.hpp"
#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/classify/weight_computer.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <iostream>

using namespace nukex;

// ── Helper: populate synthetic voxel data ──
static void fill_synthetic(ShadowBuffers& buf, int B, int C, int N,
                            std::mt19937& rng) {
    std::normal_distribution<float> gauss(0.5f, 0.05f);

    for (int vi = 0; vi < B; vi++) {
        // n_frames is [C * B] now -- one count per channel, because two
        // slots can read different caches with different frame sets. Setting
        // only [vi] would leave every channel above 0 with zero frames and
        // quietly stop testing them.
        for (int ch = 0; ch < C; ch++)
            buf.n_frames[ch * B + vi] = static_cast<uint16_t>(N);

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

    for (int ch = 0; ch < C; ch++) buf.n_frames[ch * B + 0] = 5;
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

TEST_CASE("CPU Fallback: select_pixels noise falls as 1/sqrt(N) with equal weights",
          "[gpu][fallback]") {
    // N identical samples under the CCD model, unit weights: the propagated
    // noise is sigma_sample / sqrt(N). Ten frames against a hundred must
    // therefore differ by sqrt(10). This used to be asserted on a CPU copy of
    // kernel 3 that nothing in the product called; it is asserted here on the
    // path that runs.
    auto run = [](int N) {
        const int B = 1, C = 1;
        ShadowBuffers buf;
        buf.allocate(B, C, N);
        for (int fi = 0; fi < N; fi++) {
            buf.pixel_values[fi * B] = 0.5f;
            buf.pixel_weights[fi * B] = 1.0f;
        }
        buf.n_frames[0] = static_cast<uint16_t>(N);
        buf.dist_true_signal[0] = 0.5f;
        auto fs = make_frame_stats(N);          // gain 1.5, read noise 3, keywords present
        GPUCPUFallback::select_pixels(buf, fs.data(), B, C, N);
        return buf.noise_sigma[0];
    };
    const float n10 = run(10), n100 = run(100);
    REQUIRE(n10 > 0.0f);
    REQUIRE(n100 < n10);
    REQUIRE(n10 / n100 == Catch::Approx(std::sqrt(10.0f)).epsilon(0.01));
}

TEST_CASE("CPU Fallback: predicted noise uses a robust scale, not Welford",
          "[gpu][fallback]") {
    // Without GAIN/RDNOISE keywords the noise model falls back to an
    // across-frame scale. Welford variance is NOT robust: one satellite trail
    // or cosmic ray inflates it, the predicted noise rises to meet whatever
    // the estimator produced, and the measured/predicted check goes blind.
    //
    // 21 samples: ten at 0.49, ten at 0.51, one outlier at 0.90.
    //   median 0.51, MAD 0.02  -> robust sigma = 0.02 * 1.4826 = 0.0296520
    //   predicted = sigma / sqrt(21)             = 0.0064707
    // Welford on the same samples gives sigma 0.0878584, i.e. 0.0191723 --
    // 3x larger, and driven entirely by the one bad sample.
    const int B = 1, C = 1, N = 21;
    ShadowBuffers buf;
    buf.allocate(B, C, N);

    for (int fi = 0; fi < N; fi++) {
        const float v = (fi < 10) ? 0.49f : (fi < 20 ? 0.51f : 0.90f);
        buf.pixel_values[fi * B] = v;
        buf.pixel_weights[fi * B] = 1.0f;
    }
    buf.n_frames[0] = static_cast<uint16_t>(N);
    buf.dist_true_signal[0] = 0.51f;

    // Welford of that sample set, computed exactly.
    buf.welford_mean[0] = 0.5190476f;
    buf.welford_M2[0]   = 0.1543810f;
    buf.welford_n[0]    = static_cast<uint32_t>(N);
    // Robust scale from kernel 2.
    buf.mad_out[0]      = 0.02f;

    // No noise keywords -> the across-frame fallback is what runs.
    auto fs = make_frame_stats(N);
    for (int i = 0; i < N; i++) fs[i].has_noise_keywords = false;

    GPUCPUFallback::select_pixels(buf, fs.data(), B, C, N);

    REQUIRE(buf.noise_sigma[0] == Catch::Approx(0.0064707f).epsilon(0.02));
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

// ── local_rms measures REALISED noise, and is blind to structure ──
//
// local_rms is the stack's own answer to "how noisy is it here", and the
// detection-horizon work reads it as the yardstick for what a single pixel can
// show. MAD about a local median cannot serve: on smooth structure it reports
// the structure as noise. These pin the estimator to neighbour differences,
// which cancel anything smooth.

TEST_CASE("CPU Fallback: local_rms is blind to a smooth gradient", "[gpu][fallback]") {
    // A noiseless linear ramp. There is nothing to measure: the true
    // pixel-to-pixel noise is exactly zero.
    const int W = 64, H = 64, C = 1;
    const float slope = 0.001f;
    std::vector<float> stacked(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            stacked[y * W + x] = 0.2f + slope * static_cast<float>(x);

    std::vector<float> grad(W * H), bg(W * H), rms(W * H);
    GPUCPUFallback::spatial_context(stacked.data(), W, H, C,
                                     grad.data(), bg.data(), rms.data());

    // Interior only -- window clipping at the border is a separate concern.
    for (int y = 12; y < H - 12; y++)
        for (int x = 12; x < W - 12; x++)
            REQUIRE(rms[y * W + x] == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("CPU Fallback: local_rms recovers a known noise sigma", "[gpu][fallback]") {
    const int W = 64, H = 64, C = 1;
    const float sigma = 0.01f;
    std::mt19937 rng(12345);
    std::normal_distribution<float> gauss(0.0f, sigma);

    std::vector<float> stacked(W * H);
    for (int i = 0; i < W * H; i++) stacked[i] = 0.5f + gauss(rng);

    std::vector<float> grad(W * H), bg(W * H), rms(W * H);
    GPUCPUFallback::spatial_context(stacked.data(), W, H, C,
                                     grad.data(), bg.data(), rms.data());

    double sum = 0.0; int n = 0;
    for (int y = 12; y < H - 12; y++)
        for (int x = 12; x < W - 12; x++) { sum += rms[y * W + x]; n++; }
    const double mean_rms = sum / n;

    // 15% tolerance: a 15x15 window gives ~200 difference pairs, so the MAD of
    // those differences carries real sampling scatter.
    REQUIRE(mean_rms == Catch::Approx(sigma).epsilon(0.15));
}

TEST_CASE("CPU Fallback: local_rms equals the brute-force sorted estimator exactly",
          "[gpu][fallback]") {
    // The kernel takes the pooled median of |d - median| without sorting the
    // deviations (two-pointer walks and a merge). It must equal the sorted
    // version to the bit, on data with ties and with structure.
    const int W = 40, H = 36, C = 1;
    std::mt19937 rng(77);
    std::normal_distribution<float> gauss(0.0f, 0.02f);
    std::vector<float> stacked(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float v = 0.3f + 0.002f * x + gauss(rng);
            if ((x + y) % 7 == 0) v = 0.31f;              // ties
            stacked[y * W + x] = v;
        }
    std::vector<float> grad(W * H), bg(W * H), rms(W * H);
    GPUCPUFallback::spatial_context(stacked.data(), W, H, C,
                                     grad.data(), bg.data(), rms.data());

    auto sorted_median = [](std::vector<float> v) {
        std::sort(v.begin(), v.end());
        const int n = static_cast<int>(v.size());
        return (n % 2 == 1) ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
    };
    const int R = 7, LAG = 8;
    for (int y = 0; y < H; y += 5)
        for (int x = 0; x < W; x += 7) {
            const int y0 = std::max(0, y - R), y1 = std::min(H - 1, y + R);
            const int x0 = std::max(0, x - R), x1 = std::min(W - 1, x + R);
            const int ww = x1 - x0 + 1, wh = y1 - y0 + 1;
            std::vector<float> win;
            for (int wy = y0; wy <= y1; wy++)
                for (int wx = x0; wx <= x1; wx++) win.push_back(stacked[wy * W + wx]);
            std::vector<float> dh, dv;
            for (int ry = 0; ry < wh; ry++)
                for (int rx = 0; rx + LAG < ww; rx++) dh.push_back(win[ry * ww + rx + LAG] - win[ry * ww + rx]);
            for (int rx = 0; rx < ww; rx++)
                for (int ry = 0; ry + LAG < wh; ry++) dv.push_back(win[(ry + LAG) * ww + rx] - win[ry * ww + rx]);
            std::vector<float> dev;
            if (!dh.empty()) { const float m = sorted_median(dh); for (float d : dh) dev.push_back(std::fabs(d - m)); }
            if (!dv.empty()) { const float m = sorted_median(dv); for (float d : dv) dev.push_back(std::fabs(d - m)); }
            const float expect = (dev.size() > 1) ? sorted_median(dev) * 1.4826f * 0.70710678f : 0.0f;
            INFO("pixel " << x << "," << y);
            REQUIRE(rms[y * W + x] == expect);      // exact, deliberately
        }
}

TEST_CASE("CPU Fallback: local_rms reads the MARGINAL sigma of correlated noise",
          "[gpu][fallback]") {
    // A real stack's pixels are correlated: debayering and resampling both
    // average neighbours. Here white noise of sigma s is box-averaged 3x3, so
    // every pixel's marginal sigma is s/3 while adjacent pixels share 6 of 9
    // source samples (rho = 2/3). A lag-1 difference estimator reads
    // s/3 * sqrt(1 - rho) = 0.58 * s/3 -- 42% low. The predicted noise map is
    // a marginal sigma, so the measured one must be too.
    const int W = 96, H = 96, C = 1;
    const float s = 0.03f;
    std::mt19937 rng(2024);
    std::normal_distribution<float> gauss(0.0f, s);
    std::vector<float> white((W + 2) * (H + 2));
    for (auto& v : white) v = gauss(rng);
    std::vector<float> stacked(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float acc = 0.0f;
            for (int dy = 0; dy < 3; dy++)
                for (int dx = 0; dx < 3; dx++) acc += white[(y + dy) * (W + 2) + (x + dx)];
            stacked[y * W + x] = 0.5f + acc / 9.0f;
        }
    std::vector<float> grad(W * H), bg(W * H), rms(W * H);
    GPUCPUFallback::spatial_context(stacked.data(), W, H, C,
                                     grad.data(), bg.data(), rms.data());
    double sum = 0.0; int n = 0;
    for (int y = 12; y < H - 12; y++)
        for (int x = 12; x < W - 12; x++) { sum += rms[y * W + x]; n++; }
    const double mean_rms = sum / n;
    REQUIRE(mean_rms == Catch::Approx(s / 3.0).epsilon(0.15));
}

TEST_CASE("CPU Fallback: local_rms is not inflated by a gradient under the noise",
          "[gpu][fallback]") {
    // The case that matters on real data: faint noise riding a sky gradient.
    // The gradient must not be counted as noise.
    const int W = 64, H = 64, C = 1;
    const float sigma = 0.01f;
    const float slope = 0.002f;     // 15x15 window spans 0.03 -- 3x sigma
    std::mt19937 rng(999);
    std::normal_distribution<float> gauss(0.0f, sigma);

    std::vector<float> stacked(W * H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            stacked[y * W + x] = 0.2f + slope * static_cast<float>(x) + gauss(rng);

    std::vector<float> grad(W * H), bg(W * H), rms(W * H);
    GPUCPUFallback::spatial_context(stacked.data(), W, H, C,
                                     grad.data(), bg.data(), rms.data());

    double sum = 0.0; int n = 0;
    for (int y = 12; y < H - 12; y++)
        for (int x = 12; x < W - 12; x++) { sum += rms[y * W + x]; n++; }
    const double mean_rms = sum / n;

    REQUIRE(mean_rms == Catch::Approx(sigma).epsilon(0.15));
}

// ══════════════════════════════════════════════════════════
// Batch-size invariance
//
// Phase B slices the cube into batches sized from GPU VRAM, and the shadow
// buffers are host-side vectors of that size. Before capping the batch by
// host RAM it has to be established that batch size is a staging choice and
// not a numerical one -- if it moved pixel output, every E2E golden would
// silently depend on how much VRAM the machine happened to have.
//
// Batch size is part of the SoA stride (ch * N * B + fi * B + vi), so this is
// not self-evidently true from reading the indexing.
// ══════════════════════════════════════════════════════════

namespace {

void fill_dist_inputs(ShadowBuffers& buf, int B, int C) {
    for (int ch = 0; ch < C; ch++)
        for (int vi = 0; vi < B; vi++) {
            buf.dist_true_signal[ch * B + vi] = 0.4f + 0.001f * ((vi * 7 + ch) % 97);
            buf.dist_uncertainty[ch * B + vi] = 0.01f + 0.0001f * ((vi * 13 + ch) % 51);
            buf.dist_confidence[ch * B + vi]  = 0.5f + 0.004f * ((vi * 3 + ch) % 101);
        }
}

// Copy one voxel's inputs out of `src` (batch Bs, voxel si) into `dst`
// (batch Bd, voxel di), re-striding as it goes.
void copy_voxel_inputs(const ShadowBuffers& src, int Bs, int si,
                       ShadowBuffers& dst, int Bd, int di, int C, int N) {
    for (int ch = 0; ch < C; ch++)
        dst.n_frames[ch * Bd + di] = src.n_frames[ch * Bs + si];
    for (int ch = 0; ch < C; ch++) {
        dst.welford_mean[ch * Bd + di] = src.welford_mean[ch * Bs + si];
        dst.welford_M2  [ch * Bd + di] = src.welford_M2  [ch * Bs + si];
        dst.welford_n   [ch * Bd + di] = src.welford_n   [ch * Bs + si];
        dst.dist_true_signal[ch * Bd + di] = src.dist_true_signal[ch * Bs + si];
        dst.dist_uncertainty[ch * Bd + di] = src.dist_uncertainty[ch * Bs + si];
        dst.dist_confidence [ch * Bd + di] = src.dist_confidence [ch * Bs + si];
        for (int fi = 0; fi < N; fi++)
            dst.pixel_values[ch * N * Bd + fi * Bd + di] =
                src.pixel_values[ch * N * Bs + fi * Bs + si];
    }
}

} // namespace

TEST_CASE("CPU Fallback: kernel output does not depend on batch size",
          "[gpu][fallback]") {
    const int B = 96, C = 3, N = 12;
    auto fs = make_frame_stats(N);
    WeightConfig wc;

    std::mt19937 rng(20260905u);
    ShadowBuffers full;
    full.allocate(B, C, N);
    fill_synthetic(full, B, C, N, rng);
    fill_dist_inputs(full, B, C);

    GPUCPUFallback::classify_weights(full, fs.data(), wc, B, C, N);
    GPUCPUFallback::robust_stats(full, B, C, N);
    GPUCPUFallback::select_pixels(full, fs.data(), B, C, N);

    for (int split : {48, 32, 7}) {
        INFO("batch split = " << split);
        // Re-run the identical voxels in chunks of `split` and compare.
        for (int base = 0; base < B; base += split) {
            const int count = std::min(split, B - base);

            ShadowBuffers part;
            part.allocate(count, C, N);
            for (int vi = 0; vi < count; vi++)
                copy_voxel_inputs(full, B, base + vi, part, count, vi, C, N);

            GPUCPUFallback::classify_weights(part, fs.data(), wc, count, C, N);
            GPUCPUFallback::robust_stats(part, count, C, N);
            GPUCPUFallback::select_pixels(part, fs.data(), count, C, N);

            for (int vi = 0; vi < count; vi++) {
                const int si = base + vi;
                INFO("voxel " << si);
                REQUIRE(part.cloud_frame_count[vi] == full.cloud_frame_count[si]);
                REQUIRE(part.trail_frame_count[vi] == full.trail_frame_count[si]);
                REQUIRE(part.worst_sigma_score[vi] == full.worst_sigma_score[si]);
                REQUIRE(part.best_sigma_score[vi]  == full.best_sigma_score[si]);
                REQUIRE(part.mean_weight_out[vi]   == full.mean_weight_out[si]);
                REQUIRE(part.total_exposure_out[vi]== full.total_exposure_out[si]);
                for (int ch = 0; ch < C; ch++) {
                    REQUIRE(part.mad_out[ch * count + vi]
                            == full.mad_out[ch * B + si]);
                    REQUIRE(part.biweight_midvar_out[ch * count + vi]
                            == full.biweight_midvar_out[ch * B + si]);
                    REQUIRE(part.iqr_out[ch * count + vi]
                            == full.iqr_out[ch * B + si]);
                    REQUIRE(part.output_value[ch * count + vi]
                            == full.output_value[ch * B + si]);
                    REQUIRE(part.noise_sigma[ch * count + vi]
                            == full.noise_sigma[ch * B + si]);
                    REQUIRE(part.snr_out[ch * count + vi]
                            == full.snr_out[ch * B + si]);
                }
            }
        }
    }
}

TEST_CASE("ShadowBuffers: coverage bits use the batch's own stride, not the "
          "allocated one", "[gpu][fallback][coverage]") {
    // The final batch of a cube is usually PARTIAL, so the voxel stride the
    // kernels use (count) is smaller than the stride the buffers were
    // allocated for (batch_size). Writing the coverage bits at one stride and
    // reading them at the other put them where nothing looked, and since the
    // number of batches now depends on free memory, that made pixel output
    // vary between otherwise identical runs -- but only on cubes big enough
    // to need more than one batch, which is why the two OSC corpora moved and
    // the single-batch mono ones did not.
    const int alloc_B = 100, C = 2, N = 4;
    ShadowBuffers buf;
    buf.allocate(alloc_B, C, N);
    // extract_from_cube sizes the plane; do the same here.
    buf.pixel_valid.assign((static_cast<std::size_t>(C) * N * alloc_B + 7) / 8, 0);

    const int count = 37;   // a partial final batch
    REQUIRE(count < alloc_B);

    // Mark a deterministic pattern using the BATCH stride.
    auto want = [](int ch, int fi, int vi) { return ((ch * 7 + fi * 3 + vi) % 5) != 0; };
    for (int ch = 0; ch < C; ch++)
        for (int fi = 0; fi < N; fi++)
            for (int vi = 0; vi < count; vi++)
                buf.set_sample_valid(ch, fi, vi, want(ch, fi, vi), count);

    // Read it back at the same stride: every bit must survive.
    for (int ch = 0; ch < C; ch++)
        for (int fi = 0; fi < N; fi++)
            for (int vi = 0; vi < count; vi++) {
                INFO("ch=" << ch << " fi=" << fi << " vi=" << vi);
                REQUIRE(buf.sample_valid(ch, fi, vi, count) == want(ch, fi, vi));
            }

    // And reading at the ALLOCATED stride must NOT agree, which is exactly
    // the mistake this guards: if the two ever coincide the test is vacuous.
    int disagreements = 0;
    for (int ch = 0; ch < C; ch++)
        for (int fi = 0; fi < N; fi++)
            for (int vi = 0; vi < count; vi++)
                if (buf.sample_valid(ch, fi, vi, alloc_B) != want(ch, fi, vi))
                    ++disagreements;
    REQUIRE(disagreements > 0);
}
