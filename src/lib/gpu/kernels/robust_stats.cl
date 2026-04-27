// ── NukeX v4: Kernel 2 — Robust Statistics ──────────────────────
// One work-item per (voxel, channel) pair.
// Global size = batch_size * n_channels.
//
// Computes MAD, biweight midvariance, and IQR per voxel-channel.
// Uses insertion sort in private memory (matches CPU fallback exactly).

// Include common helpers (insertion_sort_f, sorted_median_f)
// These are prepended by the kernel compilation system.

__kernel void robust_stats(
    __global const float*   pixel_values,       // [C * N * B]
    __global const ushort*  n_frames_in,        // [B]
    int n_channels,
    int max_frames,
    int batch_size,
    __global float* mad_out,                    // [C * B]
    __global float* biweight_midvar_out,        // [C * B]
    __global float* iqr_out                     // [C * B]
) {
    int gid = get_global_id(0);
    int B = batch_size;
    int N = max_frames;
    int C = n_channels;

    int vi = gid % B;
    int ch = gid / B;
    if (ch >= C || vi >= B) return;

    int nf = (int)n_frames_in[vi];
    if (nf < 2) {
        mad_out[ch * B + vi] = 0.0f;
        biweight_midvar_out[ch * B + vi] = 0.0f;
        iqr_out[ch * B + vi] = 0.0f;
        return;
    }

    int n = min(nf, GPU_MAX_FRAMES);

    // Load values into private memory
    float vals[GPU_MAX_FRAMES];
    float sorted[GPU_MAX_FRAMES];
    for (int fi = 0; fi < n; fi++)
        vals[fi] = pixel_values[ch * N * B + fi * B + vi];

    // ── MAD ──
    for (int i = 0; i < n; i++) sorted[i] = vals[i];
    insertion_sort_f(sorted, n);
    float med = sorted_median_f(sorted, n);

    float abs_devs[GPU_MAX_FRAMES];
    for (int i = 0; i < n; i++)
        abs_devs[i] = fabs(vals[i] - med);
    insertion_sort_f(abs_devs, n);
    float mad_val = sorted_median_f(abs_devs, n);
    mad_out[ch * B + vi] = mad_val;

    // ── IQR ──
    int q1_idx = n / 4;
    int q3_idx = (3 * n) / 4;
    iqr_out[ch * B + vi] = (n >= 4) ? sorted[q3_idx] - sorted[q1_idx] : 0.0f;

    // ── Biweight midvariance ──
    float c_bw = 9.0f;
    if (mad_val < 1.0e-30f) {
        biweight_midvar_out[ch * B + vi] = 0.0f;
        return;
    }

    float num = 0.0f, den = 0.0f;
    for (int i = 0; i < n; i++) {
        float u = (vals[i] - med) / (c_bw * mad_val);
        if (fabs(u) < 1.0f) {
            float u2 = u * u;
            float diff = vals[i] - med;
            float one_minus_u2 = 1.0f - u2;
            num += diff * diff * one_minus_u2 * one_minus_u2 * one_minus_u2 * one_minus_u2;
            den += one_minus_u2 * (1.0f - 5.0f * u2);
        }
    }

    float bwmv = (fabs(den) > 1.0e-30f)
        ? (float)n * num / (den * den)
        : 0.0f;
    biweight_midvar_out[ch * B + vi] = bwmv;
}
