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
    __global const ushort*  n_frames_in,        // [C * B]
    // Frame-level noise model
    // Per CHANNEL: see classify_weights.cl.
    __global const float*   frame_read_noise,   // [C * N]
    __global const float*   frame_gain,         // [C * N]
    __global const uchar*   frame_has_noise_kw, // [C * N]
    // Phase A's per-frame normalisation, per CHANNEL, so the Poisson term
    // below can be evaluated on the raw value it is only meaningful for.
    __global const float*   frame_norm_scale,   // [C * N]
    __global const float*   frame_norm_offset,  // [C * N]
    // Across-frame fallback scale. `mad` is kernel 2's robust scale and is
    // preferred; Welford is kept only for degenerate voxels where the robust
    // scale is zero.
    __global const float*   welford_M2,         // [C * B]
    __global const uint*    welford_n,          // [C * B]
    __global const float*   mad,                // [C * B]
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

    int nf = (int)n_frames_in[ch * B + vi];
    float out_val = dist_true_signal[ch * B + vi];

    // Across-frame fallback scale (mirrors the CPU fallback).
    //
    // Welford variance is NOT robust: one satellite trail or cosmic ray
    // inflates it, so the predicted noise rises to meet whatever the estimator
    // produced and the measured-vs-predicted check goes blind.
    float w_M2 = welford_M2[ch * B + vi];
    uint  w_n  = welford_n[ch * B + vi];
    float welford_var = (w_n > 1)
        ? max(0.0f, w_M2) / (float)(w_n - 1)
        : 0.0f;
    float robust_sigma = mad[ch * B + vi] * 1.4826f;
    float fallback_var = (robust_sigma > 0.0f)
                       ? robust_sigma * robust_sigma
                       : welford_var;

    // Noise propagation
    float weight_sum = 0.0f;
    float variance_sum = 0.0f;

    for (int fi = 0; fi < nf; fi++) {
        float w = pixel_weights[ch * N * B + fi * B + vi];
        float value = pixel_values[ch * N * B + fi * B + vi];

        float sigma2;
        if (frame_has_noise_kw[ch * N + fi]) {
            float g = max(frame_gain[ch * N + fi], 1.0e-10f);
            float rn = frame_read_noise[ch * N + fi];
            // Undo Phase A's normalisation before the Poisson term: shot
            // noise belongs to the photons actually collected, so evaluate
            // it on the raw value and carry it back through
            // Var(a*x + b) = a^2 Var(x). At the identity (1, 0) this is
            // bit-for-bit the un-normalised expression, so an uncorrected
            // batch cannot move. Must match gpu_cpu_fallback.cpp exactly.
            float a = frame_norm_scale[ch * N + fi];
            float b = frame_norm_offset[ch * N + fi];
            if (!(a > 0.0f)) { a = 1.0f; b = 0.0f; }
            float value_adu = ((value - b) / a) * 65535.0f;
            float shot_var = value_adu / g;
            float read_var = (rn * rn) / (g * g);
            sigma2 = a * a * (shot_var + read_var) / (65535.0f * 65535.0f);
        } else {
            sigma2 = fallback_var;
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
