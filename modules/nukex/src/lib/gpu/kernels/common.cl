// ── NukeX v5 OpenCL Common Definitions ──────────────────────────
// Shared constants and helper functions for all GPU kernels.
// All kernels use float (IEEE 754 single precision).

#ifndef NUKEX_COMMON_CL
#define NUKEX_COMMON_CL

// Maximum frames supported by GPU kernels.
// Voxels with more frames than this fall back to CPU.
#define GPU_MAX_FRAMES 512

// ── Insertion sort for small arrays (matches CPU fallback exactly) ──
// Used by robust_stats for median computation.
// O(n²) but fast for n ≤ 512 in private memory.
inline void insertion_sort_f(float* arr, int n) {
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

// ── Median of sorted array (matches CPU fallback) ──
inline float sorted_median_f(float* sorted, int n) {
    if (n <= 0) return 0.0f;
    if (n % 2 == 1) return sorted[n / 2];
    return 0.5f * (sorted[n / 2 - 1] + sorted[n / 2]);
}

#endif // NUKEX_COMMON_CL


// One bit per (channel, frame slot, voxel), same index as pixel_values.
// 1 = that frame covered this pixel; 0 = the warp left it outside the source,
// so the stored value is an absence and not a dark measurement.
inline int sample_is_valid(__global const uchar* pixel_valid,
                           int ch, int fi, int vi, int N, int B) {
    ulong b = ((ulong)ch * N + fi) * B + vi;
    return (pixel_valid[b >> 3] >> (b & 7)) & 1u;
}
