#include "catch_amalgamated.hpp"
#include "nukex/gpu/gpu_context.hpp"
#include "nukex/gpu/gpu_kernels.hpp"
#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/gpu/gpu_cpu_fallback.hpp"
#include "nukex/gpu/gpu_executor.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/classify/weight_computer.hpp"
#include <cmath>
#include <random>
#include <iostream>

using namespace nukex;

// Tolerance: 1 ULP for IEEE 754 single precision ≈ 1.19e-7 at value 1.0
// We use a slightly larger margin to account for accumulation across frames
static constexpr float GPU_TOL = 1e-4f;

static void fill_synthetic(ShadowBuffers& buf, int B, int C, int N,
                            std::mt19937& rng) {
    std::normal_distribution<float> gauss(0.5f, 0.05f);
    for (int vi = 0; vi < B; vi++) {
        buf.n_frames[vi] = static_cast<uint16_t>(N);
        for (int ch = 0; ch < C; ch++) {
            float sum = 0.0f;
            for (int fi = 0; fi < N; fi++) {
                float val = std::clamp(gauss(rng), 0.0f, 1.0f);
                buf.pixel_values[ch * N * B + fi * B + vi] = val;
                sum += val;
            }
            float mean = sum / N;
            float sum2 = 0.0f;
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
        fs[i].psf_weight = 0.9f + 0.01f * i;
        fs[i].cloud_score = 1.0f - 0.001f * i;
        fs[i].exposure = 300.0f;
        fs[i].gain = 1.5f;
        fs[i].read_noise = 3.0f;
        fs[i].has_noise_keywords = true;
    }
    return fs;
}

TEST_CASE("GPU Agreement: classify_weights GPU == CPU", "[gpu][agreement]") {
    auto ctx = GPUContext::create();
    if (!ctx.is_gpu_available()) { SKIP("No GPU available"); }

    GPUKernels kernels;
    REQUIRE(kernels.compile(ctx));

    int B = 200, C = 3, N = 50;
    std::mt19937 rng(42);
    auto fs = make_frame_stats(N);
    WeightConfig wc;

    // Run CPU path
    ShadowBuffers cpu_buf;
    cpu_buf.allocate(B, C, N);
    fill_synthetic(cpu_buf, B, C, N, rng);

    // Run GPU path with identical input
    ShadowBuffers gpu_buf;
    gpu_buf.allocate(B, C, N);
    rng.seed(42);  // Reset RNG
    fill_synthetic(gpu_buf, B, C, N, rng);

    GPUCPUFallback::classify_weights(cpu_buf, fs.data(), wc, B, C, N);

    // GPU dispatch
    GPUExecutor gpu_exec;
    gpu_exec.execute_batch_gpu(gpu_buf, fs.data(), wc, B, C, N);

    // Compare weights
    int mismatches = 0;
    float max_diff = 0.0f;
    for (int ch = 0; ch < C; ch++) {
        for (int fi = 0; fi < N; fi++) {
            for (int vi = 0; vi < B; vi++) {
                int idx = ch * N * B + fi * B + vi;
                float cpu_w = cpu_buf.pixel_weights[idx];
                float gpu_w = gpu_buf.pixel_weights[idx];
                float diff = std::fabs(cpu_w - gpu_w);
                if (diff > GPU_TOL) mismatches++;
                if (diff > max_diff) max_diff = diff;
            }
        }
    }

    std::cout << "classify_weights: max_diff=" << max_diff
              << " mismatches=" << mismatches << "/" << (C * N * B) << "\n";
    REQUIRE(mismatches == 0);

    // Compare classification summaries
    for (int vi = 0; vi < B; vi++) {
        REQUIRE(std::fabs(cpu_buf.mean_weight_out[vi] - gpu_buf.mean_weight_out[vi]) < GPU_TOL);
        REQUIRE(std::fabs(cpu_buf.total_exposure_out[vi] - gpu_buf.total_exposure_out[vi]) < GPU_TOL);
        REQUIRE(std::fabs(cpu_buf.worst_sigma_score[vi] - gpu_buf.worst_sigma_score[vi]) < GPU_TOL);
    }

    std::cout << "classify_weights: GPU == CPU ✓\n";
}

TEST_CASE("GPU Agreement: robust_stats GPU == CPU", "[gpu][agreement]") {
    auto ctx = GPUContext::create();
    if (!ctx.is_gpu_available()) { SKIP("No GPU available"); }

    GPUKernels kernels;
    REQUIRE(kernels.compile(ctx));

    int B = 100, C = 3, N = 30;
    std::mt19937 rng(42);

    ShadowBuffers cpu_buf, gpu_buf;
    cpu_buf.allocate(B, C, N);
    gpu_buf.allocate(B, C, N);
    fill_synthetic(cpu_buf, B, C, N, rng);
    rng.seed(42);
    fill_synthetic(gpu_buf, B, C, N, rng);

    GPUCPUFallback::robust_stats(cpu_buf, B, C, N);

    // GPU dispatch (robust_stats only — need to dispatch via executor)
    // For now, use CPU fallback on GPU buf too and compare
    // (GPU kernel dispatch for robust_stats alone needs the executor wiring)
    auto fs = make_frame_stats(N);
    WeightConfig wc;
    GPUExecutor gpu_exec;
    gpu_exec.execute_batch_gpu(gpu_buf, fs.data(), wc, B, C, N);

    // Compare MAD
    float max_mad_diff = 0.0f;
    for (int ch = 0; ch < C; ch++) {
        for (int vi = 0; vi < B; vi++) {
            float diff = std::fabs(cpu_buf.mad_out[ch * B + vi] - gpu_buf.mad_out[ch * B + vi]);
            if (diff > max_mad_diff) max_mad_diff = diff;
        }
    }

    std::cout << "robust_stats MAD: max_diff=" << max_mad_diff << "\n";
    // Sorting-based algorithms may have larger tolerance due to insertion sort order
    REQUIRE(max_mad_diff < 1e-3f);

    // Compare IQR
    float max_iqr_diff = 0.0f;
    for (int ch = 0; ch < C; ch++) {
        for (int vi = 0; vi < B; vi++) {
            float diff = std::fabs(cpu_buf.iqr_out[ch * B + vi] - gpu_buf.iqr_out[ch * B + vi]);
            if (diff > max_iqr_diff) max_iqr_diff = diff;
        }
    }
    std::cout << "robust_stats IQR: max_diff=" << max_iqr_diff << "\n";
    REQUIRE(max_iqr_diff < 1e-3f);

    std::cout << "robust_stats: GPU == CPU ✓\n";
}

TEST_CASE("GPU Agreement: select_pixels GPU == CPU", "[gpu][agreement]") {
    auto ctx = GPUContext::create();
    if (!ctx.is_gpu_available()) { SKIP("No GPU available"); }

    int B = 100, C = 3, N = 30;
    std::mt19937 rng(77);
    auto fs = make_frame_stats(N);
    WeightConfig wc;

    // Prepare identical buffers
    ShadowBuffers cpu_buf, gpu_buf;
    cpu_buf.allocate(B, C, N);
    gpu_buf.allocate(B, C, N);
    fill_synthetic(cpu_buf, B, C, N, rng);
    rng.seed(77);
    fill_synthetic(gpu_buf, B, C, N, rng);

    // Run kernels 1+2 on CPU for both (so weights are identical)
    GPUCPUFallback::classify_weights(cpu_buf, fs.data(), wc, B, C, N);
    GPUCPUFallback::classify_weights(gpu_buf, fs.data(), wc, B, C, N);

    // Set distribution results (simulate CPU fitting)
    for (int ch = 0; ch < C; ch++)
        for (int vi = 0; vi < B; vi++) {
            float sig = cpu_buf.welford_mean[ch * B + vi];
            cpu_buf.dist_true_signal[ch * B + vi] = sig;
            gpu_buf.dist_true_signal[ch * B + vi] = sig;
        }

    // CPU path
    GPUCPUFallback::select_pixels(cpu_buf, fs.data(), B, C, N);

    // GPU path
    GPUExecutor gpu_exec;
    gpu_exec.execute_select_gpu(gpu_buf, fs.data(), B, C, N);

    // Compare
    float max_val_diff = 0.0f, max_noise_diff = 0.0f, max_snr_diff = 0.0f;
    for (int ch = 0; ch < C; ch++) {
        for (int vi = 0; vi < B; vi++) {
            int idx = ch * B + vi;
            max_val_diff = std::max(max_val_diff,
                std::fabs(cpu_buf.output_value[idx] - gpu_buf.output_value[idx]));
            max_noise_diff = std::max(max_noise_diff,
                std::fabs(cpu_buf.noise_sigma[idx] - gpu_buf.noise_sigma[idx]));
            max_snr_diff = std::max(max_snr_diff,
                std::fabs(cpu_buf.snr_out[idx] - gpu_buf.snr_out[idx]));
        }
    }

    std::cout << "select_pixels: max_val_diff=" << max_val_diff
              << " max_noise_diff=" << max_noise_diff
              << " max_snr_diff=" << max_snr_diff << "\n";

    REQUIRE(max_val_diff < GPU_TOL);
    REQUIRE(max_noise_diff < GPU_TOL);
    REQUIRE(max_snr_diff < 0.1f);  // SNR can amplify small noise differences

    std::cout << "select_pixels: GPU == CPU ✓\n";
}

TEST_CASE("GPU Agreement: spatial_context GPU == CPU", "[gpu][agreement]") {
    auto ctx = GPUContext::create();
    if (!ctx.is_gpu_available()) { SKIP("No GPU available"); }

    // Use a moderate-sized synthetic image
    int W = 64, H = 64, C = 3;
    Image stacked(W, H, C);
    std::mt19937 rng(99);
    std::normal_distribution<float> gauss(0.3f, 0.05f);
    for (int ch = 0; ch < C; ch++)
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                stacked.at(x, y, ch) = std::clamp(gauss(rng), 0.0f, 1.0f);
    // Add a bright spot for gradient testing
    for (int y = 28; y < 36; y++)
        for (int x = 28; x < 36; x++)
            for (int ch = 0; ch < C; ch++)
                stacked.at(x, y, ch) = 0.9f;

    int npix = W * H;
    std::vector<float> cpu_grad(npix), cpu_bg(npix), cpu_rms(npix);
    std::vector<float> gpu_grad(npix), gpu_bg(npix), gpu_rms(npix);

    // CPU path
    GPUCPUFallback::spatial_context(stacked.data(), W, H, C,
        cpu_grad.data(), cpu_bg.data(), cpu_rms.data());

    // GPU path
    GPUExecutor gpu_exec;
    gpu_exec.execute_spatial_gpu(stacked, gpu_grad.data(), gpu_bg.data(), gpu_rms.data());

    // Compare
    float max_grad_diff = 0.0f, max_bg_diff = 0.0f, max_rms_diff = 0.0f;
    for (int i = 0; i < npix; i++) {
        max_grad_diff = std::max(max_grad_diff, std::fabs(cpu_grad[i] - gpu_grad[i]));
        max_bg_diff = std::max(max_bg_diff, std::fabs(cpu_bg[i] - gpu_bg[i]));
        max_rms_diff = std::max(max_rms_diff, std::fabs(cpu_rms[i] - gpu_rms[i]));
    }

    std::cout << "spatial_context: max_grad_diff=" << max_grad_diff
              << " max_bg_diff=" << max_bg_diff
              << " max_rms_diff=" << max_rms_diff << "\n";

    // Gradient uses only basic arithmetic — should be very close
    REQUIRE(max_grad_diff < 1e-4f);
    // Biweight uses iterative float accumulation — GPU uses float, CPU uses double
    // Allow slightly more tolerance for background/RMS
    REQUIRE(max_bg_diff < 1e-3f);
    REQUIRE(max_rms_diff < 1e-3f);

    std::cout << "spatial_context: GPU == CPU ✓\n";
}
