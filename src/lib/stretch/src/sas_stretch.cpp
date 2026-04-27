#include "nukex/stretch/sas_stretch.hpp"
#include "nukex/stretch/ghs_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace nukex {

float SASStretch::fit_D(float local_median) const {
    // Find D such that GHS(local_median; D, b, SP=0, LP=0, HP=1) ≈ target_median.
    // Use bisection since the GHS scalar function is monotone in D.

    if (local_median <= 1e-10f) return min_D;  // Dark tile — use minimum stretch
    if (local_median >= target_median) return min_D;  // Already bright enough

    GHSStretch ghs;
    ghs.b = ghs_b;
    ghs.SP = 0.0f;
    ghs.LP = 0.0f;
    ghs.HP = 1.0f;

    float lo = 0.0f, hi = max_D;

    // Bisect: find D where GHS(local_median) = target_median
    for (int iter = 0; iter < 40; iter++) {
        float mid = (lo + hi) * 0.5f;
        ghs.D = mid;
        float stretched = ghs.apply_scalar(local_median);

        if (stretched < target_median) {
            lo = mid;
        } else {
            hi = mid;
        }
    }

    return std::clamp((lo + hi) * 0.5f, min_D, max_D);
}

void SASStretch::apply(Image& img) const {
    int w = img.width();
    int h = img.height();
    int nch = img.n_channels();

    // Step 1: Compute luminance for tile statistics and stretching
    int npix = w * h;
    std::vector<float> lum(npix);
    if (nch >= 3) {
        for (int i = 0; i < npix; i++) {
            lum[i] = 0.2126f * img.channel_data(0)[i]
                   + 0.7152f * img.channel_data(1)[i]
                   + 0.0722f * img.channel_data(2)[i];
        }
    } else {
        for (int i = 0; i < npix; i++)
            lum[i] = img.channel_data(0)[i];
    }

    // Step 2: Tile decomposition
    int stride = std::max(1, static_cast<int>(tile_size * (1.0f - tile_overlap)));
    int nx_tiles = (w + stride - 1) / stride;
    int ny_tiles = (h + stride - 1) / stride;

    // Per-tile: compute center, local median, fitted D
    struct TileInfo {
        float cx, cy;   // Tile center
        float D;        // Fitted GHS intensity
    };
    std::vector<TileInfo> tiles;
    tiles.reserve(nx_tiles * ny_tiles);

    for (int ty = 0; ty < ny_tiles; ty++) {
        for (int tx = 0; tx < nx_tiles; tx++) {
            int x0 = tx * stride;
            int y0 = ty * stride;
            int x1 = std::min(x0 + tile_size, w);
            int y1 = std::min(y0 + tile_size, h);

            // Compute local median from luminance
            int tile_npix = (x1 - x0) * (y1 - y0);
            std::vector<float> tile_vals(tile_npix);
            int idx = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++)
                    tile_vals[idx++] = lum[y * w + x];

            std::nth_element(tile_vals.begin(), tile_vals.begin() + idx / 2,
                           tile_vals.begin() + idx);
            float local_median = tile_vals[idx / 2];

            TileInfo ti;
            ti.cx = (x0 + x1) * 0.5f;
            ti.cy = (y0 + y1) * 0.5f;
            ti.D = fit_D(local_median);
            tiles.push_back(ti);
        }
    }

    // Step 3: For each pixel, compute Gaussian-weighted blend of tile stretches
    float sigma = tile_size * 0.5f;
    float inv_2sigma2 = 1.0f / (2.0f * sigma * sigma);

    // Output buffers
    std::vector<float> out_lum(npix, 0.0f);
    std::vector<float> weight_sum(npix, 0.0f);

    // Pre-build a GHS for each tile and apply to the full luminance
    // (optimization: only apply within the tile's influence radius)
    float influence_radius = sigma * 3.0f;  // 3-sigma cutoff

    for (const auto& ti : tiles) {
        // Determine the bounding box of this tile's influence
        int bx0 = std::max(0, static_cast<int>(ti.cx - influence_radius));
        int by0 = std::max(0, static_cast<int>(ti.cy - influence_radius));
        int bx1 = std::min(w, static_cast<int>(ti.cx + influence_radius + 1));
        int by1 = std::min(h, static_cast<int>(ti.cy + influence_radius + 1));

        GHSStretch ghs;
        ghs.D = ti.D;
        ghs.b = ghs_b;
        ghs.SP = 0.0f;
        ghs.LP = 0.0f;
        ghs.HP = 1.0f;

        for (int y = by0; y < by1; y++) {
            float dy = y - ti.cy;
            float dy2 = dy * dy;
            for (int x = bx0; x < bx1; x++) {
                float dx = x - ti.cx;
                float dist2 = dx * dx + dy2;
                float wt = std::exp(-dist2 * inv_2sigma2);

                float L = lum[y * w + x];
                float L_stretched = ghs.apply_scalar(L);

                int pi = y * w + x;
                out_lum[pi] += wt * L_stretched;
                weight_sum[pi] += wt;
            }
        }
    }

    // Step 4: Normalize and apply to image channels
    if (luminance_only && nch >= 3) {
        // Scale RGB by L_new / L_old ratio
        for (int i = 0; i < npix; i++) {
            float L_old = lum[i];
            if (L_old < 1e-10f || weight_sum[i] < 1e-10f) continue;

            float L_new = out_lum[i] / weight_sum[i];
            float scale = L_new / L_old;

            for (int c = 0; c < nch; c++) {
                img.channel_data(c)[i] = std::clamp(
                    img.channel_data(c)[i] * scale, 0.0f, 1.0f);
            }
        }
    } else {
        // Single channel or per-channel mode
        for (int c = 0; c < nch; c++) {
            float* data = img.channel_data(c);
            // Recompute with per-channel data (simplified: use luminance blend for all)
            for (int i = 0; i < npix; i++) {
                if (weight_sum[i] < 1e-10f) continue;
                float old_val = data[i];
                if (old_val < 1e-10f) continue;
                float L_old = lum[i];
                if (L_old < 1e-10f) continue;
                float L_new = out_lum[i] / weight_sum[i];
                data[i] = std::clamp(old_val * (L_new / L_old), 0.0f, 1.0f);
            }
        }
    }

    clamp_image(img);
}

} // namespace nukex
