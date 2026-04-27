#include "nukex/stretch/clahe_stretch.hpp"
#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include <numeric>

namespace nukex {

std::vector<float> CLAHEStretch::build_tile_cdf(
    const float* data, int w, int /*h*/,
    int x0, int y0, int x1, int y1) const {

    int tile_pixels = (x1 - x0) * (y1 - y0);

    // Build histogram
    std::vector<int> hist(n_bins, 0);
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float val = std::clamp(data[y * w + x], 0.0f, 1.0f);
            int bin = static_cast<int>(val * (n_bins - 1));
            hist[bin]++;
        }
    }

    // Clip histogram and redistribute excess
    int clip_count = static_cast<int>(clip_limit * tile_pixels / n_bins);
    int excess = 0;
    for (int i = 0; i < n_bins; i++) {
        if (hist[i] > clip_count) {
            excess += hist[i] - clip_count;
            hist[i] = clip_count;
        }
    }

    // Redistribute excess evenly
    int per_bin = excess / n_bins;
    int remainder = excess % n_bins;
    for (int i = 0; i < n_bins; i++) {
        hist[i] += per_bin;
        if (i < remainder) hist[i]++;
    }

    // Build CDF
    std::vector<float> cdf(n_bins);
    float inv_total = 1.0f / static_cast<float>(tile_pixels);
    int cumsum = 0;
    for (int i = 0; i < n_bins; i++) {
        cumsum += hist[i];
        cdf[i] = cumsum * inv_total;
    }

    return cdf;
}

void CLAHEStretch::apply(Image& img) const {
    int w = img.width();
    int h = img.height();
    int nch = img.n_channels();

    // Determine which data to process
    std::vector<float> lum;
    float* process_data;

    if (luminance_only && nch >= 3) {
        // Extract luminance
        int n = w * h;
        lum.resize(n);
        for (int i = 0; i < n; i++) {
            lum[i] = 0.2126f * img.channel_data(0)[i]
                   + 0.7152f * img.channel_data(1)[i]
                   + 0.0722f * img.channel_data(2)[i];
        }
        process_data = lum.data();
    } else {
        // Single channel
        process_data = img.channel_data(0);
    }

    // Compute tile boundaries
    int tw = (w + tile_cols - 1) / tile_cols;
    int th = (h + tile_rows - 1) / tile_rows;

    // Build CDF for each tile
    std::vector<std::vector<float>> tile_cdfs(tile_cols * tile_rows);
    // Tile center positions
    std::vector<float> tile_cx(tile_cols), tile_cy(tile_rows);

    for (int ty = 0; ty < tile_rows; ty++) {
        int y0 = ty * th;
        int y1 = std::min(y0 + th, h);
        tile_cy[ty] = (y0 + y1) * 0.5f;
        for (int tx = 0; tx < tile_cols; tx++) {
            int x0 = tx * tw;
            int x1 = std::min(x0 + tw, w);
            tile_cx[tx] = (x0 + x1) * 0.5f;
            tile_cdfs[ty * tile_cols + tx] = build_tile_cdf(process_data, w, h, x0, y0, x1, y1);
        }
    }

    // Apply: for each pixel, bilinear interpolation of 4 nearest tile CDFs
    auto lookup_cdf = [this](const std::vector<float>& cdf, float val) -> float {
        int bin = static_cast<int>(std::clamp(val, 0.0f, 1.0f) * (n_bins - 1));
        return cdf[bin];
    };

    std::vector<float> output(w * h);

    for (int y = 0; y < h; y++) {
        // Find the two nearest tile rows
        int ty0 = -1, ty1 = -1;
        for (int ty = 0; ty < tile_rows - 1; ty++) {
            if (y >= tile_cy[ty] && y < tile_cy[ty + 1]) {
                ty0 = ty; ty1 = ty + 1; break;
            }
        }
        if (ty0 < 0) {
            if (y < tile_cy[0]) { ty0 = 0; ty1 = 0; }
            else { ty0 = tile_rows - 1; ty1 = tile_rows - 1; }
        }

        float beta = (ty0 == ty1) ? 0.0f :
            (y - tile_cy[ty0]) / (tile_cy[ty1] - tile_cy[ty0]);
        beta = std::clamp(beta, 0.0f, 1.0f);

        for (int x = 0; x < w; x++) {
            // Find the two nearest tile columns
            int tx0 = -1, tx1 = -1;
            for (int tx = 0; tx < tile_cols - 1; tx++) {
                if (x >= tile_cx[tx] && x < tile_cx[tx + 1]) {
                    tx0 = tx; tx1 = tx + 1; break;
                }
            }
            if (tx0 < 0) {
                if (x < tile_cx[0]) { tx0 = 0; tx1 = 0; }
                else { tx0 = tile_cols - 1; tx1 = tile_cols - 1; }
            }

            float alpha = (tx0 == tx1) ? 0.0f :
                (x - tile_cx[tx0]) / (tile_cx[tx1] - tile_cx[tx0]);
            alpha = std::clamp(alpha, 0.0f, 1.0f);

            float val = process_data[y * w + x];

            // Bilinear interpolation of 4 tile CDFs
            float v_tl = lookup_cdf(tile_cdfs[ty0 * tile_cols + tx0], val);
            float v_tr = lookup_cdf(tile_cdfs[ty0 * tile_cols + tx1], val);
            float v_bl = lookup_cdf(tile_cdfs[ty1 * tile_cols + tx0], val);
            float v_br = lookup_cdf(tile_cdfs[ty1 * tile_cols + tx1], val);

            float v_top = v_tl * (1.0f - alpha) + v_tr * alpha;
            float v_bot = v_bl * (1.0f - alpha) + v_br * alpha;
            output[y * w + x] = v_top * (1.0f - beta) + v_bot * beta;
        }
    }

    // Apply result
    if (luminance_only && nch >= 3) {
        // Scale RGB by L_new / L_old ratio
        for (int i = 0; i < w * h; i++) {
            float L_old = lum[i];
            if (L_old < 1e-10f) continue;
            float L_new = output[i];
            float scale = L_new / L_old;
            for (int c = 0; c < nch; c++) {
                img.channel_data(c)[i] = std::clamp(
                    img.channel_data(c)[i] * scale, 0.0f, 1.0f);
            }
        }
    } else {
        // Direct replacement
        float* data = img.channel_data(0);
        for (int i = 0; i < w * h; i++)
            data[i] = output[i];
    }

    clamp_image(img);
}

std::map<std::string, std::pair<float, float>> CLAHEStretch::param_bounds() const {
    return {
        {"clip_limit", {0.5f,  5.0f}},
        {"tile_cols",  {2.0f, 32.0f}},
        {"tile_rows",  {2.0f, 32.0f}},
    };
}

bool CLAHEStretch::set_param(const std::string& n, float v) {
    if (n == "clip_limit") { clip_limit = v;                           return true; }
    if (n == "tile_cols")  { tile_cols  = static_cast<int>(v + 0.5f);  return true; }
    if (n == "tile_rows")  { tile_rows  = static_cast<int>(v + 0.5f);  return true; }
    return false;
}

std::optional<float> CLAHEStretch::get_param(const std::string& n) const {
    if (n == "clip_limit") return clip_limit;
    if (n == "tile_cols")  return static_cast<float>(tile_cols);
    if (n == "tile_rows")  return static_cast<float>(tile_rows);
    return std::nullopt;
}

} // namespace nukex
