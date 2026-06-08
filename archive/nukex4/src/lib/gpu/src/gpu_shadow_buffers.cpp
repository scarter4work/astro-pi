#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include <cstring>

namespace nukex {

void ShadowBuffers::allocate(int bs, int nc, int mf) {
    batch_size = bs;
    n_channels = nc;
    max_frames = mf;

    int B = bs, C = nc, N = mf;

    // Input buffers
    welford_mean.resize(C * B, 0.0f);
    welford_M2.resize(C * B, 0.0f);
    welford_n.resize(C * B, 0);
    pixel_values.resize(C * N * B, 0.0f);
    n_frames.resize(B, 0);

    // Intermediate
    pixel_weights.resize(C * N * B, 0.0f);

    // Classification output
    cloud_frame_count.resize(B, 0);
    trail_frame_count.resize(B, 0);
    worst_sigma_score.resize(B, 0.0f);
    best_sigma_score.resize(B, 0.0f);
    mean_weight_out.resize(B, 0.0f);
    total_exposure_out.resize(B, 0.0f);

    // Robust stats output
    mad_out.resize(C * B, 0.0f);
    biweight_midvar_out.resize(C * B, 0.0f);
    iqr_out.resize(C * B, 0.0f);

    // Distribution input
    dist_true_signal.resize(C * B, 0.0f);
    dist_uncertainty.resize(C * B, 0.0f);
    dist_confidence.resize(C * B, 0.0f);

    // Selection output
    output_value.resize(C * B, 0.0f);
    noise_sigma.resize(C * B, 0.0f);
    snr_out.resize(C * B, 0.0f);
}

void ShadowBuffers::extract_from_cube(
    const Cube& cube, const FrameCache& cache,
    int start_voxel, int count, int nc) {

    int B = count;
    int C = nc;
    int N = max_frames;
    int w = cube.width;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        const auto& voxel = cube.at(px, py);

        n_frames[vi] = voxel.n_frames;

        for (int ch = 0; ch < C; ch++) {
            welford_mean[ch * B + vi] = voxel.welford[ch].mean;
            welford_M2[ch * B + vi]   = voxel.welford[ch].M2;
            welford_n[ch * B + vi]    = voxel.welford[ch].n;

            // Read all frame values for this pixel-channel at once
            float frame_vals[GPU_MAX_FRAMES];
            int nf_read = cache.read_pixel(px, py, ch, frame_vals);
            int n_copy = std::min(nf_read, std::min(static_cast<int>(voxel.n_frames), N));
            for (int fi = 0; fi < n_copy; fi++) {
                pixel_values[ch * N * B + fi * B + vi] = frame_vals[fi];
            }
        }
    }
}

void ShadowBuffers::writeback_classification(
    Cube& cube, int start_voxel, int count, int nc) const {

    int B = count;
    int C = nc;
    int w = cube.width;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        auto& voxel = cube.at(px, py);

        voxel.cloud_frame_count = cloud_frame_count[vi];
        voxel.trail_frame_count = trail_frame_count[vi];
        voxel.worst_sigma_score = worst_sigma_score[vi];
        voxel.best_sigma_score  = best_sigma_score[vi];
        voxel.mean_weight       = mean_weight_out[vi];
        voxel.total_exposure    = total_exposure_out[vi];

        for (int ch = 0; ch < C; ch++) {
            voxel.mad[ch]                  = mad_out[ch * B + vi];
            voxel.biweight_midvariance[ch] = biweight_midvar_out[ch * B + vi];
            voxel.iqr[ch]                  = iqr_out[ch * B + vi];
        }
    }
}

void ShadowBuffers::extract_distributions(
    const Cube& cube, int start_voxel, int count, int nc) {

    int B = count;
    int C = nc;
    int w = cube.width;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        const auto& voxel = cube.at(px, py);

        for (int ch = 0; ch < C; ch++) {
            dist_true_signal[ch * B + vi] = voxel.distribution[ch].true_signal_estimate;
            dist_uncertainty[ch * B + vi] = voxel.distribution[ch].signal_uncertainty;
            dist_confidence[ch * B + vi]  = voxel.distribution[ch].confidence;
        }
    }
}

void ShadowBuffers::writeback_selection(
    Cube& cube, int start_voxel, int count, int nc,
    float* output_image, float* noise_image) const {

    int B = count;
    int C = nc;
    int w = cube.width;
    int h = cube.height;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        auto& voxel = cube.at(px, py);

        for (int ch = 0; ch < C; ch++) {
            float val = output_value[ch * B + vi];
            float noise = noise_sigma[ch * B + vi];
            float snr = snr_out[ch * B + vi];

            voxel.snr[ch] = snr;

            // Write to output images (channel-by-channel, row-major)
            if (output_image)
                output_image[ch * w * h + py * w + px] = val;
            if (noise_image)
                noise_image[ch * w * h + py * w + px] = noise;
        }
    }
}

} // namespace nukex
