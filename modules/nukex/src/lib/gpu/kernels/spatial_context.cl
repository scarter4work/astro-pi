// ── NukeX v5: Kernel 4 — Spatial Context ────────────────────────
// One work-item per pixel. Global size = width * height.
//
// Computes Sobel gradient magnitude, local background (biweight location),
// and local RMS (neighbour differences) over a 15×15 window.
//
// Operates on the stacked output image (channel-by-channel layout),
// NOT on voxels. This runs once after all voxel processing is complete.
//
// Matches gpu_cpu_fallback.cpp::spatial_context() exactly.

// Include common helpers
// (prepended by kernel compilation system)

#define WINDOW_RADIUS 7
#define MAX_WINDOW_SIZE 225  // (2*7+1)^2

__kernel void spatial_context(
    __global const float* stacked_image,    // [C * W * H]
    int width,
    int height,
    int n_channels,
    int lum_mode,                            // 0: single plane lum_c0; 1: rec709 of c0,c1,c2
    int lum_c0,
    int lum_c1,
    int lum_c2,
    __global float* gradient_mag,           // [W * H]
    __global float* local_background,       // [W * H]
    __global float* local_rms               // [W * H]
) {
    int gid = get_global_id(0);
    if (gid >= width * height) return;

    int x = gid % width;
    int y = gid / width;
    int W = width;
    int H = height;
    int C = n_channels;

    // ── Sobel gradient (max across channels) ──
    float max_grad = 0.0f;
    for (int ch = 0; ch < C; ch++) {
        __global const float* img = stacked_image + ch * W * H;

        int xm = max(0, x - 1), xp = min(W - 1, x + 1);
        int ym = max(0, y - 1), yp = min(H - 1, y + 1);

        float gx = -img[ym * W + xm] + img[ym * W + xp]
                  - 2.0f * img[y * W + xm] + 2.0f * img[y * W + xp]
                  - img[yp * W + xm] + img[yp * W + xp];

        float gy = -img[ym * W + xm] - 2.0f * img[ym * W + x] - img[ym * W + xp]
                  + img[yp * W + xm] + 2.0f * img[yp * W + x] + img[yp * W + xp];

        float g = sqrt(gx * gx + gy * gy);
        if (g > max_grad) max_grad = g;
    }
    gradient_mag[gid] = max_grad;

    // ── Local background and RMS (15×15 window, biweight) ──
    float window[MAX_WINDOW_SIZE];
    int wn = 0;

    int y0 = max(0, y - WINDOW_RADIUS);
    int y1 = min(H - 1, y + WINDOW_RADIUS);
    int x0 = max(0, x - WINDOW_RADIUS);
    int x1 = min(W - 1, x + WINDOW_RADIUS);

    for (int wy = y0; wy <= y1; wy++) {
        for (int wx = x0; wx <= x1; wx++) {
            float lum;
            if (lum_mode == 1) {
                lum = 0.2126f * stacked_image[lum_c0 * W * H + wy * W + wx]
                    + 0.7152f * stacked_image[lum_c1 * W * H + wy * W + wx]
                    + 0.0722f * stacked_image[lum_c2 * W * H + wy * W + wx];
            } else {
                lum = stacked_image[lum_c0 * W * H + wy * W + wx];
            }
            window[wn++] = lum;
        }
    }

    // Biweight location (iterative, matches CPU fallback)
    float c_bw = 6.0f;
    int max_iter = 10;
    float tol = 1.0e-7f;

    float sorted_win[MAX_WINDOW_SIZE];
    for (int i = 0; i < wn; i++) sorted_win[i] = window[i];
    insertion_sort_f(sorted_win, wn);
    float location = sorted_median_f(sorted_win, wn);

    // MAD of window
    float abs_devs_win[MAX_WINDOW_SIZE];
    for (int i = 0; i < wn; i++)
        abs_devs_win[i] = fabs(window[i] - location);
    insertion_sort_f(abs_devs_win, wn);
    float mad_val = sorted_median_f(abs_devs_win, wn);

    if (mad_val > 1.0e-30f) {
        float scale = mad_val * 1.4826f;

        for (int iter = 0; iter < max_iter; iter++) {
            float num = 0.0f, den = 0.0f;
            for (int i = 0; i < wn; i++) {
                float u = (window[i] - location) / (c_bw * scale);
                if (fabs(u) < 1.0f) {
                    float u2 = u * u;
                    float w = (1.0f - u2) * (1.0f - u2);
                    num += w * window[i];
                    den += w;
                }
            }
            if (den < 1.0e-30f) break;
            float new_loc = num / den;
            if (fabs(new_loc - location) < tol * scale) {
                location = new_loc;
                break;
            }
            location = new_loc;
        }
    }

    local_background[gid] = location;

    // ── Local RMS from neighbour differences (mirrors the CPU fallback) ──
    //
    // MAD about the local median reports smooth STRUCTURE as noise: on a
    // noiseless ramp of slope s it returns 5.93*s. Differencing pixels a
    // fixed lag apart cancels anything smooth. The window is gathered
    // row-major.
    // Lag 8, both directions: adjacent pixels are correlated by debayering
    // and resampling (measured rho 0.34 mono, 0.6-0.7 OSC), and the
    // difference-based sigma flattens near lag 8. Mirrors the CPU fallback.
    const int LAG = 8;
    int win_w = x1 - x0 + 1;
    int win_h = y1 - y0 + 1;
    float dh[MAX_WINDOW_SIZE], dv[MAX_WINDOW_SIZE];
    int nh = 0, nv = 0;
    for (int ry = 0; ry < win_h; ry++)
        for (int rx = 0; rx + LAG < win_w; rx++)
            dh[nh++] = window[ry * win_w + rx + LAG] - window[ry * win_w + rx];
    for (int rx = 0; rx < win_w; rx++)
        for (int ry = 0; ry + LAG < win_h; ry++)
            dv[nv++] = window[(ry + LAG) * win_w + rx] - window[ry * win_w + rx];

    // Each direction is centred on its OWN median before pooling: a smooth
    // ramp gives one constant difference per direction, different between
    // directions, so pooling raw differences would read the ramp as scatter.
    // Each direction is sorted once (for its median); its absolute deviations
    // come out ascending by a two-pointer walk (m - s[i] leftward, s[i] - m
    // rightward), and the pooled median is a merge: O(n) in place of the
    // O(n^2) sort of 210 pooled deviations, bit-identically. Mirrors the CPU.
    float rms_val = 0.0f;
    if (nh + nv > 1) {
        float devh[MAX_WINDOW_SIZE], devv[MAX_WINDOW_SIZE];
        if (nh > 0) {
            insertion_sort_f(dh, nh);
            float m = sorted_median_f(dh, nh);
            int split = 0;
            while (split < nh && dh[split] <= m) ++split;
            int l = split - 1, r = split, k = 0;
            while (k < nh) {
                float dl = (l >= 0) ? (m - dh[l]) : 3.402823466e+38f;
                float dr = (r < nh) ? (dh[r] - m) : 3.402823466e+38f;
                if (l >= 0 && (r >= nh || dl <= dr)) { devh[k++] = dl; --l; }
                else                                 { devh[k++] = dr; ++r; }
            }
        }
        if (nv > 0) {
            insertion_sort_f(dv, nv);
            float m = sorted_median_f(dv, nv);
            int split = 0;
            while (split < nv && dv[split] <= m) ++split;
            int l = split - 1, r = split, k = 0;
            while (k < nv) {
                float dl = (l >= 0) ? (m - dv[l]) : 3.402823466e+38f;
                float dr = (r < nv) ? (dv[r] - m) : 3.402823466e+38f;
                if (l >= 0 && (r >= nv || dl <= dr)) { devv[k++] = dl; --l; }
                else                                 { devv[k++] = dr; ++r; }
            }
        }
        // Median of the union of the two ascending runs, same convention as
        // sorted_median_f (middle element, or the mean of the two middles).
        int n = nh + nv, i = 0, j = 0;
        float prev = 0.0f, cur = 0.0f;
        for (int k = 0; k <= n / 2; ++k) {
            prev = cur;
            if (j >= nv || (i < nh && devh[i] <= devv[j])) cur = devh[i++];
            else                                            cur = devv[j++];
        }
        float pooled = (n % 2 == 1) ? cur : 0.5f * (prev + cur);
        // 1.4826 converts MAD to a Gaussian sigma; 1/sqrt(2) undoes the
        // differencing of two independent samples.
        rms_val = pooled * 1.4826f * 0.70710678f;
    }
    local_rms[gid] = rms_val;
}
