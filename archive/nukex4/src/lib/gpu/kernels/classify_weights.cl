// ── NukeX v4: Kernel 1 — Weight Computation + Classification ────
// One work-item per voxel. Processes all channels and all frames.
//
// Memory layout (channel-major, coalesced):
//   welford_mean[ch * B + vi]
//   pixel_values[ch * N * B + fi * B + vi]
//
// Matches gpu_cpu_fallback.cpp::classify_weights() exactly.

__kernel void classify_weights(
    __global const float*   welford_mean,       // [C * B]
    __global const float*   welford_M2,         // [C * B]
    __global const uint*    welford_n,          // [C * B]
    __global const float*   pixel_values,       // [C * N * B]
    __global const ushort*  n_frames_in,        // [B]
    // Frame-level constants (read-only, shared across all work-items)
    __global const float*   frame_weight,       // [N]
    __global const float*   psf_weight,         // [N]
    __global const float*   cloud_score,        // [N]
    __global const float*   frame_exposure,     // [N]
    // WeightConfig scalars
    float sigma_threshold,
    float sigma_scale,
    float weight_floor,
    // Dimensions
    int n_channels,
    int max_frames,
    int batch_size,
    // Outputs
    __global float*  pixel_weights_out,         // [C * N * B]
    __global ushort* cloud_count_out,           // [B]
    __global ushort* trail_count_out,           // [B]
    __global float*  worst_sigma_out,           // [B]
    __global float*  best_sigma_out,            // [B]
    __global float*  mean_weight_out,           // [B]
    __global float*  total_exposure_out         // [B]
) {
    int vi = get_global_id(0);
    if (vi >= batch_size) return;

    int B = batch_size;
    int N = max_frames;
    int C = n_channels;
    int nf = (int)n_frames_in[vi];
    if (nf == 0) return;

    float worst_sigma = 0.0f;
    float best_sigma = 1.0e30f;
    float weight_sum = 0.0f;
    float total_exp = 0.0f;
    ushort cloud_count = 0;
    ushort trail_count = 0;

    float inv_sigma_scale2 = 0.5f / (sigma_scale * sigma_scale);

    for (int ch = 0; ch < C; ch++) {
        float w_mean = welford_mean[ch * B + vi];
        float w_M2   = welford_M2[ch * B + vi];
        uint  w_n    = welford_n[ch * B + vi];

        float variance = (w_n > 1) ? max(0.0f, w_M2) / (float)(w_n - 1) : 0.0f;
        float stddev = sqrt(variance);

        for (int fi = 0; fi < nf; fi++) {
            float value = pixel_values[ch * N * B + fi * B + vi];

            float w = frame_weight[fi] * psf_weight[fi];

            if (stddev > 1.0e-30f) {
                float sigma_score = fabs(value - w_mean) / stddev;
                float excess = max(0.0f, sigma_score - sigma_threshold);
                float sigma_factor = exp(-excess * excess * inv_sigma_scale2);
                w *= sigma_factor;

                if (ch == 0) {
                    if (sigma_score > worst_sigma) worst_sigma = sigma_score;
                    if (sigma_score < best_sigma) best_sigma = sigma_score;
                }
            }

            w *= cloud_score[fi];
            w = max(w, weight_floor);

            pixel_weights_out[ch * N * B + fi * B + vi] = w;

            if (ch == 0) {
                weight_sum += w;
                total_exp += frame_exposure[fi];
                if (cloud_score[fi] < 0.5f) cloud_count++;
            }
        }
    }

    cloud_count_out[vi] = cloud_count;
    trail_count_out[vi] = trail_count;
    worst_sigma_out[vi] = worst_sigma;
    best_sigma_out[vi]  = (best_sigma < 1.0e29f) ? best_sigma : 0.0f;
    mean_weight_out[vi] = (nf > 0) ? weight_sum / (float)nf : 0.0f;
    total_exposure_out[vi] = total_exp;
}
