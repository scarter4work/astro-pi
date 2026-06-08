#include "nukex/gpu/gpu_cpu_fallback.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

// ── Helper: insertion sort for small arrays (matches OpenCL kernel) ──
static void insertion_sort(float* arr, int n) {
    for (int i = 1; i < n; i++) {
        float key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

// ── Helper: median via insertion sort (matches OpenCL kernel) ──
static float sorted_median(float* sorted, int n) {
    if (n <= 0) return 0.0f;
    if (n % 2 == 1) return sorted[n / 2];
    return 0.5f * (sorted[n / 2 - 1] + sorted[n / 2]);
}

// ══════════════════════════════════════════════════════════════════════
// Kernel 1: classify_weights
// ══════════════════════════════════════════════════════════════════════

void GPUCPUFallback::classify_weights(
    ShadowBuffers& buf,
    const FrameStats* frame_stats,
    const WeightConfig& config,
    int batch_size, int n_channels, int n_frames) {

    int B = batch_size;
    int N = n_frames;
    int C = n_channels;

    for (int vi = 0; vi < B; vi++) {
        int nf = buf.n_frames[vi];
        if (nf == 0) continue;

        float worst_sigma = 0.0f;
        float best_sigma = 1e30f;
        float weight_sum = 0.0f;
        float total_exp = 0.0f;
        uint16_t cloud_count = 0;
        uint16_t trail_count = 0;

        for (int ch = 0; ch < C; ch++) {
            // Welford stats for this voxel-channel
            float w_mean = buf.welford_mean[ch * B + vi];
            float w_M2   = buf.welford_M2[ch * B + vi];
            uint32_t w_n = buf.welford_n[ch * B + vi];

            float variance = (w_n > 1) ? std::max(0.0f, w_M2) / static_cast<float>(w_n - 1) : 0.0f;
            float stddev = std::sqrt(variance);

            for (int fi = 0; fi < nf; fi++) {
                float value = buf.pixel_values[ch * N * B + fi * B + vi];

                // Weight computation (mirrors weight_computer.cpp)
                float w = frame_stats[fi].frame_weight * frame_stats[fi].psf_weight;

                if (stddev > 1e-30f) {
                    float sigma_score = std::fabs(value - w_mean) / stddev;
                    float excess = std::max(0.0f, sigma_score - config.sigma_threshold);
                    float sigma_factor = std::exp(-0.5f * excess * excess
                                                  / (config.sigma_scale * config.sigma_scale));
                    w *= sigma_factor;

                    // Track sigma scores for channel 0 only
                    if (ch == 0) {
                        if (sigma_score > worst_sigma) worst_sigma = sigma_score;
                        if (sigma_score < best_sigma) best_sigma = sigma_score;
                    }
                }

                w *= frame_stats[fi].cloud_score;
                w = std::max(w, config.weight_floor);

                buf.pixel_weights[ch * N * B + fi * B + vi] = w;

                // Accumulate summaries from channel 0
                if (ch == 0) {
                    weight_sum += w;
                    total_exp += frame_stats[fi].exposure;
                    if (frame_stats[fi].cloud_score < 0.5f) cloud_count++;
                }
            }
        }

        buf.cloud_frame_count[vi] = cloud_count;
        buf.trail_frame_count[vi] = trail_count;
        buf.worst_sigma_score[vi] = worst_sigma;
        buf.best_sigma_score[vi]  = (best_sigma < 1e29f) ? best_sigma : 0.0f;
        buf.mean_weight_out[vi]   = (nf > 0) ? weight_sum / static_cast<float>(nf) : 0.0f;
        buf.total_exposure_out[vi] = total_exp;
    }
}

// ══════════════════════════════════════════════════════════════════════
// Kernel 2: robust_stats
// ══════════════════════════════════════════════════════════════════════

void GPUCPUFallback::robust_stats(
    ShadowBuffers& buf,
    int batch_size, int n_channels, int n_frames) {

    int B = batch_size;
    int N = n_frames;
    int C = n_channels;

    for (int vi = 0; vi < B; vi++) {
        int nf = buf.n_frames[vi];
        if (nf < 2) {
            for (int ch = 0; ch < C; ch++) {
                buf.mad_out[ch * B + vi] = 0.0f;
                buf.biweight_midvar_out[ch * B + vi] = 0.0f;
                buf.iqr_out[ch * B + vi] = 0.0f;
            }
            continue;
        }

        for (int ch = 0; ch < C; ch++) {
            // Collect values for this voxel-channel
            float vals[GPU_MAX_FRAMES];
            float sorted[GPU_MAX_FRAMES];
            int n = std::min(nf, static_cast<int>(GPU_MAX_FRAMES));

            for (int fi = 0; fi < n; fi++)
                vals[fi] = buf.pixel_values[ch * N * B + fi * B + vi];

            // ── MAD ──
            // Sort to find median
            for (int i = 0; i < n; i++) sorted[i] = vals[i];
            insertion_sort(sorted, n);
            float med = sorted_median(sorted, n);

            // Absolute deviations from median
            float abs_devs[GPU_MAX_FRAMES];
            for (int i = 0; i < n; i++)
                abs_devs[i] = std::fabs(vals[i] - med);
            insertion_sort(abs_devs, n);
            float mad_val = sorted_median(abs_devs, n);
            buf.mad_out[ch * B + vi] = mad_val;

            // ── IQR ──
            int q1_idx = n / 4;
            int q3_idx = (3 * n) / 4;
            buf.iqr_out[ch * B + vi] = (n >= 4) ? sorted[q3_idx] - sorted[q1_idx] : 0.0f;

            // ── Biweight midvariance ──
            constexpr float c_bw = 9.0f;
            if (mad_val < 1e-30f) {
                buf.biweight_midvar_out[ch * B + vi] = 0.0f;
                continue;
            }

            double num = 0.0, den = 0.0;
            for (int i = 0; i < n; i++) {
                float u = (vals[i] - med) / (c_bw * mad_val);
                if (std::fabs(u) < 1.0f) {
                    float u2 = u * u;
                    float diff = vals[i] - med;
                    num += diff * diff * std::pow(1.0f - u2, 4);
                    den += (1.0f - u2) * (1.0f - 5.0f * u2);
                }
            }
            float bwmv = (std::fabs(den) > 1e-30)
                ? static_cast<float>(n * num / (den * den))
                : 0.0f;
            buf.biweight_midvar_out[ch * B + vi] = bwmv;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════
// Kernel 3: select_pixels
// ══════════════════════════════════════════════════════════════════════

void GPUCPUFallback::select_pixels(
    ShadowBuffers& buf,
    const FrameStats* frame_stats,
    int batch_size, int n_channels, int n_frames) {

    int B = batch_size;
    int N = n_frames;
    int C = n_channels;

    for (int vi = 0; vi < B; vi++) {
        int nf = buf.n_frames[vi];

        for (int ch = 0; ch < C; ch++) {
            float out_val = buf.dist_true_signal[ch * B + vi];

            // Noise propagation (mirrors pixel_selector.cpp)
            double weight_sum = 0.0;
            double variance_sum = 0.0;

            // Compute welford variance for fallback
            float w_M2 = buf.welford_M2[ch * B + vi];
            uint32_t w_n = buf.welford_n[ch * B + vi];
            float welford_var = (w_n > 1)
                ? std::max(0.0f, w_M2) / static_cast<float>(w_n - 1)
                : 0.0f;

            for (int fi = 0; fi < nf; fi++) {
                float w = buf.pixel_weights[ch * N * B + fi * B + vi];
                float value = buf.pixel_values[ch * N * B + fi * B + vi];

                // CCD noise model or Welford fallback
                float sigma2;
                if (frame_stats[fi].has_noise_keywords) {
                    float g = std::max(frame_stats[fi].gain, 1e-10f);
                    float rn = frame_stats[fi].read_noise;
                    float value_adu = value * 65535.0f;
                    float shot_var = value_adu / g;
                    float read_var = (rn * rn) / (g * g);
                    sigma2 = (shot_var + read_var) / (65535.0f * 65535.0f);
                } else {
                    sigma2 = welford_var;
                }

                weight_sum += static_cast<double>(w);
                variance_sum += static_cast<double>(w) * static_cast<double>(w)
                              * static_cast<double>(sigma2);
            }

            float noise = 0.0f;
            if (weight_sum > 1e-30) {
                noise = static_cast<float>(std::sqrt(variance_sum) / weight_sum);
            }

            float snr = (noise > 1e-30f)
                ? std::clamp(out_val / noise, 0.0f, 9999.0f)
                : 0.0f;

            buf.output_value[ch * B + vi] = out_val;
            buf.noise_sigma[ch * B + vi] = noise;
            buf.snr_out[ch * B + vi] = snr;
        }
    }
}

// ══════════════════════════════════════════════════════════════════════
// Kernel 4: spatial_context
// ══════════════════════════════════════════════════════════════════════

void GPUCPUFallback::spatial_context(
    const float* stacked_data,
    int width, int height, int n_channels,
    float* gradient_mag,
    float* local_background,
    float* local_rms) {

    constexpr int WINDOW_RADIUS = 7;  // 15x15 window

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int pi = y * width + x;

            // ── Sobel gradient (max across channels) ──
            float max_grad = 0.0f;
            for (int ch = 0; ch < n_channels; ch++) {
                const float* img = stacked_data + ch * width * height;

                // Clamp neighbors
                int xm = std::max(0, x - 1), xp = std::min(width - 1, x + 1);
                int ym = std::max(0, y - 1), yp = std::min(height - 1, y + 1);

                float gx = -img[ym * width + xm] + img[ym * width + xp]
                          - 2.0f * img[y * width + xm] + 2.0f * img[y * width + xp]
                          - img[yp * width + xm] + img[yp * width + xp];

                float gy = -img[ym * width + xm] - 2.0f * img[ym * width + x] - img[ym * width + xp]
                          + img[yp * width + xm] + 2.0f * img[yp * width + x] + img[yp * width + xp];

                float g = std::sqrt(gx * gx + gy * gy);
                if (g > max_grad) max_grad = g;
            }
            gradient_mag[pi] = max_grad;

            // ── Local background and RMS (15×15 window, biweight) ──
            // Collect luminance values in the window
            float window[225];  // 15×15 max
            int wn = 0;

            int y0 = std::max(0, y - WINDOW_RADIUS);
            int y1 = std::min(height - 1, y + WINDOW_RADIUS);
            int x0 = std::max(0, x - WINDOW_RADIUS);
            int x1 = std::min(width - 1, x + WINDOW_RADIUS);

            for (int wy = y0; wy <= y1; wy++) {
                for (int wx = x0; wx <= x1; wx++) {
                    float lum = 0.0f;
                    if (n_channels >= 3) {
                        lum = 0.2126f * stacked_data[0 * width * height + wy * width + wx]
                            + 0.7152f * stacked_data[1 * width * height + wy * width + wx]
                            + 0.0722f * stacked_data[2 * width * height + wy * width + wx];
                    } else {
                        lum = stacked_data[wy * width + wx];
                    }
                    window[wn++] = lum;
                }
            }

            // Biweight location (iterative, matches robust_stats.cpp)
            constexpr float c_bw = 6.0f;
            constexpr int max_iter = 10;
            constexpr float tol = 1e-7f;

            float sorted_win[225];
            for (int i = 0; i < wn; i++) sorted_win[i] = window[i];
            insertion_sort(sorted_win, wn);
            float location = sorted_median(sorted_win, wn);

            // MAD of window
            float abs_devs_win[225];
            for (int i = 0; i < wn; i++)
                abs_devs_win[i] = std::fabs(window[i] - location);
            insertion_sort(abs_devs_win, wn);
            float mad_val = sorted_median(abs_devs_win, wn);

            if (mad_val > 1e-30f) {
                float scale = mad_val * 1.4826f;

                for (int iter = 0; iter < max_iter; iter++) {
                    double num = 0.0, den = 0.0;
                    for (int i = 0; i < wn; i++) {
                        float u = (window[i] - location) / (c_bw * scale);
                        if (std::fabs(u) < 1.0f) {
                            float u2 = u * u;
                            float w = (1.0f - u2) * (1.0f - u2);
                            num += w * window[i];
                            den += w;
                        }
                    }
                    if (den < 1e-30) break;
                    float new_loc = static_cast<float>(num / den);
                    if (std::fabs(new_loc - location) < tol * scale) {
                        location = new_loc;
                        break;
                    }
                    location = new_loc;
                }
            }

            local_background[pi] = location;
            local_rms[pi] = mad_val * 1.4826f;  // MAD * 1.4826 ≈ σ for Gaussian
        }
    }
}

} // namespace nukex
