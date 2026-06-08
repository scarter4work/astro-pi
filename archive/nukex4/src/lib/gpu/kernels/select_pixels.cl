// ── NukeX v4: Kernel 3 — Pixel Selection + Noise Propagation ────
// One work-item per (voxel, channel) pair.
// Global size = batch_size * n_channels.
//
// Reads the fitted distribution's true_signal_estimate (from CPU fitting),
// computes noise propagation using CCD noise model or Welford fallback.
// Matches gpu_cpu_fallback.cpp::select_pixels() exactly.

__kernel void select_pixels(
    __global const float*   dist_true_signal,   // [C * B]
    __global const float*   pixel_values,       // [C * N * B]
    __global const float*   pixel_weights,      // [C * N * B]
    __global const ushort*  n_frames_in,        // [B]
    // Frame-level noise model
    __global const float*   frame_read_noise,   // [N]
    __global const float*   frame_gain,         // [N]
    __global const uchar*   frame_has_noise_kw, // [N]
    // Welford variance for fallback
    __global const float*   welford_M2,         // [C * B]
    __global const uint*    welford_n,          // [C * B]
    // Dimensions
    int n_channels,
    int max_frames,
    int batch_size,
    // Outputs
    __global float* output_value,               // [C * B]
    __global float* noise_sigma,                // [C * B]
    __global float* snr_out                     // [C * B]
) {
    int gid = get_global_id(0);
    int B = batch_size;
    int N = max_frames;
    int C = n_channels;

    int vi = gid % B;
    int ch = gid / B;
    if (ch >= C || vi >= B) return;

    int nf = (int)n_frames_in[vi];
    float out_val = dist_true_signal[ch * B + vi];

    // Compute welford variance for fallback
    float w_M2 = welford_M2[ch * B + vi];
    uint  w_n  = welford_n[ch * B + vi];
    float welford_var = (w_n > 1)
        ? max(0.0f, w_M2) / (float)(w_n - 1)
        : 0.0f;

    // Noise propagation
    float weight_sum = 0.0f;
    float variance_sum = 0.0f;

    for (int fi = 0; fi < nf; fi++) {
        float w = pixel_weights[ch * N * B + fi * B + vi];
        float value = pixel_values[ch * N * B + fi * B + vi];

        float sigma2;
        if (frame_has_noise_kw[fi]) {
            float g = max(frame_gain[fi], 1.0e-10f);
            float rn = frame_read_noise[fi];
            float value_adu = value * 65535.0f;
            float shot_var = value_adu / g;
            float read_var = (rn * rn) / (g * g);
            sigma2 = (shot_var + read_var) / (65535.0f * 65535.0f);
        } else {
            sigma2 = welford_var;
        }

        weight_sum += w;
        variance_sum += w * w * sigma2;
    }

    float noise = 0.0f;
    if (weight_sum > 1.0e-30f) {
        noise = sqrt(variance_sum) / weight_sum;
    }

    float snr = (noise > 1.0e-30f)
        ? clamp(out_val / noise, 0.0f, 9999.0f)
        : 0.0f;

    output_value[ch * B + vi] = out_val;
    noise_sigma[ch * B + vi] = noise;
    snr_out[ch * B + vi] = snr;
}
