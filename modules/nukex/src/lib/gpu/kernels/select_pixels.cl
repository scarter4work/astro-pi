// ── NukeX v5: Kernel 3 — Pixel Selection + Noise Propagation ────
// One work-item per (voxel, channel) pair.
// Global size = batch_size * n_channels.
//
// Reads the fitted distribution's true_signal_estimate (from CPU fitting),
// computes noise propagation using CCD noise model or Welford fallback.
// Matches gpu_cpu_fallback.cpp::select_pixels() exactly.

__kernel void select_pixels(
    __global const float*   dist_true_signal,   // [C * B]
    __global const uchar*   dist_converged,     // [C * B] — 1=converged, 0=fit failed (n<3)
    __global const float*   pixel_values,       // [C * N * B]
    __global const float*   pixel_weights,      // [C * N * B]
    __global const ushort*  n_frames_in,        // [B] — per-voxel union count (unused; kept for arg parity)
    __global const ushort*  channel_n_frames,   // [C] — per-channel real-sample count
    __global const int*     channel_frame_remap,// [C * N] — (ch,pos)→global frame index
    // Frame-level noise model
    __global const float*   frame_read_noise,   // [NG] global frame count, indexed by gf
    __global const float*   frame_gain,         // [NG] global frame count, indexed by gf
    __global const uchar*   frame_has_noise_kw, // [NG] global frame count, indexed by gf
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
    __global float* snr_out,                    // [C * B]
    __global uchar* fallback_flag               // [C * B] — 1 where median fallback fired
) {
    int gid = get_global_id(0);
    int B = batch_size;
    int N = max_frames;
    int C = n_channels;

    int vi = gid % B;
    int ch = gid / B;
    if (ch >= C || vi >= B) return;

    // Per-channel real-sample count (was the shared per-voxel scalar).
    int nf = (int)channel_n_frames[ch];
    float out_val = dist_true_signal[ch * B + vi];
    fallback_flag[ch * B + vi] = 0;

    // Sparse-coverage fallback: mirrors gpu_cpu_fallback.cpp::select_pixels()
    // exactly. When the Phase B model-selection fit did not converge for
    // this voxel-channel (dist_converged == 0 — e.g. KDEFitter's hard n<3
    // floor), dist_true_signal is a zeroed default and must not be trusted
    // as the stacked value. Recombine via the median of the raw per-frame
    // samples instead, and flag it so the fallback is observable.
    if (!dist_converged[ch * B + vi]) {
        int n = min(nf, GPU_MAX_FRAMES);
        float vals[GPU_MAX_FRAMES];
        for (int fi = 0; fi < n; fi++)
            vals[fi] = pixel_values[ch * N * B + fi * B + vi];
        if (n > 0) {
            insertion_sort_f(vals, n);
            out_val = sorted_median_f(vals, n);
        }
        fallback_flag[ch * B + vi] = 1;
    }

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

        // Global frame index of this channel's fi-th real sample.
        int gf = channel_frame_remap[ch * N + fi];

        float sigma2;
        if (frame_has_noise_kw[gf]) {
            float g = max(frame_gain[gf], 1.0e-10f);
            float rn = frame_read_noise[gf];
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
