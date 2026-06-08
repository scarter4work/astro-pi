#include "nukex/io/debayer.hpp"
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace nukex {

void DebayerEngine::equalize_bayer_background(Image& raw, BayerPattern pattern) {
    if (raw.n_channels() != 1) return;
    if (pattern == BayerPattern::NONE) return;

    int w = raw.width();
    int h = raw.height();

    // Determine sub-pixel offsets for this pattern
    // offsets[color] = {x_offset, y_offset} within each 2x2 super-pixel
    // For RGGB: R=(0,0), Gr=(1,0), Gb=(0,1), B=(1,1)
    int offsets[4][2];
    switch (pattern) {
        case BayerPattern::RGGB:
            offsets[0][0]=0; offsets[0][1]=0;  // R
            offsets[1][0]=1; offsets[1][1]=0;  // Gr
            offsets[2][0]=0; offsets[2][1]=1;  // Gb
            offsets[3][0]=1; offsets[3][1]=1;  // B
            break;
        case BayerPattern::BGGR:
            offsets[0][0]=1; offsets[0][1]=1;  // R
            offsets[1][0]=0; offsets[1][1]=1;  // Gr
            offsets[2][0]=1; offsets[2][1]=0;  // Gb
            offsets[3][0]=0; offsets[3][1]=0;  // B
            break;
        case BayerPattern::GRBG:
            offsets[0][0]=1; offsets[0][1]=0;  // R
            offsets[1][0]=0; offsets[1][1]=0;  // Gr
            offsets[2][0]=1; offsets[2][1]=1;  // Gb
            offsets[3][0]=0; offsets[3][1]=1;  // B
            break;
        case BayerPattern::GBRG:
            offsets[0][0]=0; offsets[0][1]=1;  // R
            offsets[1][0]=1; offsets[1][1]=1;  // Gr
            offsets[2][0]=0; offsets[2][1]=0;  // Gb
            offsets[3][0]=1; offsets[3][1]=0;  // B
            break;
        default: return;
    }

    // Collect each sub-channel and compute its median
    int half_w = w / 2;
    int half_h = h / 2;
    int sub_n = half_w * half_h;

    float sub_medians[4];
    for (int s = 0; s < 4; s++) {
        std::vector<float> vals(sub_n);
        int ox = offsets[s][0], oy = offsets[s][1];
        int idx = 0;
        for (int y = oy; y < h - 1; y += 2)
            for (int x = ox; x < w - 1; x += 2)
                vals[idx++] = raw.at(x, y, 0);
        int n = idx;
        std::nth_element(vals.begin(), vals.begin() + n/2, vals.begin() + n);
        sub_medians[s] = vals[n/2];
    }

    // Global median (average of the 4 sub-medians)
    float global_med = (sub_medians[0] + sub_medians[1] +
                        sub_medians[2] + sub_medians[3]) * 0.25f;

    // Apply correction: subtract sub-median, add global median
    for (int s = 0; s < 4; s++) {
        float correction = global_med - sub_medians[s];
        int ox = offsets[s][0], oy = offsets[s][1];
        for (int y = oy; y < h - 1; y += 2)
            for (int x = ox; x < w - 1; x += 2)
                raw.at(x, y, 0) += correction;
    }
}

Image DebayerEngine::debayer(const Image& raw, BayerPattern pattern) {
    if (raw.n_channels() != 1) {
        throw std::invalid_argument("DebayerEngine: input must be single-channel");
    }
    if (pattern == BayerPattern::NONE) {
        throw std::invalid_argument("DebayerEngine: BayerPattern::NONE is not debayerable");
    }

    Image rgb(raw.width(), raw.height(), 3);

    switch (pattern) {
        case BayerPattern::RGGB: debayer_rggb(raw, rgb); break;
        case BayerPattern::BGGR: debayer_bggr(raw, rgb); break;
        case BayerPattern::GRBG: debayer_grbg(raw, rgb); break;
        case BayerPattern::GBRG: debayer_gbrg(raw, rgb); break;
        default: break;
    }

    return rgb;
}

// Helper: clamp coordinates to image bounds
static inline int clamp_coord(int v, int max_val) {
    return (v < 0) ? 0 : (v >= max_val ? max_val - 1 : v);
}

// Helper: read raw pixel with boundary clamping
static inline float raw_at(const Image& raw, int x, int y) {
    int cx = clamp_coord(x, raw.width());
    int cy = clamp_coord(y, raw.height());
    return raw.at(cx, cy, 0);
}

/// Bilinear debayer for RGGB pattern.
///
/// RGGB 2x2 super-pixel layout:
///   (even_x, even_y) = R
///   (odd_x,  even_y) = G (on R row)
///   (even_x, odd_y)  = G (on B row)
///   (odd_x,  odd_y)  = B
///
/// For each pixel position, the color that IS present is read directly.
/// The two missing colors are interpolated from neighbors using bilinear averaging.
void DebayerEngine::debayer_rggb(const Image& raw, Image& rgb) {
    int w = raw.width();
    int h = raw.height();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);

            if (even_x && even_y) {
                // R pixel: R is direct, G from 4 neighbors, B from 4 diagonal neighbors
                r = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                b = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else if (!even_x && even_y) {
                // G pixel on R row: G direct, R from left/right, B from top/bottom
                g = raw_at(raw, x, y);
                r = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y)) * 0.5f;
                b = (raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.5f;
            } else if (even_x && !even_y) {
                // G pixel on B row: G direct, B from left/right, R from top/bottom
                g = raw_at(raw, x, y);
                b = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y)) * 0.5f;
                r = (raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.5f;
            } else {
                // B pixel: B direct, G from 4 neighbors, R from 4 diagonals
                b = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                r = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            }

            rgb.at(x, y, 0) = r;
            rgb.at(x, y, 1) = g;
            rgb.at(x, y, 2) = b;
        }
    }
}

/// BGGR is RGGB rotated: swap R<->B positions.
void DebayerEngine::debayer_bggr(const Image& raw, Image& rgb) {
    int w = raw.width();
    int h = raw.height();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);

            if (even_x && even_y) {
                // B pixel
                b = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                r = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else if (!even_x && even_y) {
                g = raw_at(raw, x, y);
                b = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y)) * 0.5f;
                r = (raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.5f;
            } else if (even_x && !even_y) {
                g = raw_at(raw, x, y);
                r = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y)) * 0.5f;
                b = (raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.5f;
            } else {
                r = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                b = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            }

            rgb.at(x, y, 0) = r;
            rgb.at(x, y, 1) = g;
            rgb.at(x, y, 2) = b;
        }
    }
}

/// GRBG: top-left is G on R row.
void DebayerEngine::debayer_grbg(const Image& raw, Image& rgb) {
    int w = raw.width();
    int h = raw.height();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);

            if (even_x && even_y) {
                // G on R row
                g = raw_at(raw, x, y);
                r = (raw_at(raw, x+1, y) + raw_at(raw, x-1, y)) * 0.5f;
                b = (raw_at(raw, x, y+1) + raw_at(raw, x, y-1)) * 0.5f;
            } else if (!even_x && even_y) {
                // R pixel
                r = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                b = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else if (even_x && !even_y) {
                // B pixel
                b = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                r = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else {
                // G on B row
                g = raw_at(raw, x, y);
                b = (raw_at(raw, x+1, y) + raw_at(raw, x-1, y)) * 0.5f;
                r = (raw_at(raw, x, y+1) + raw_at(raw, x, y-1)) * 0.5f;
            }

            rgb.at(x, y, 0) = r;
            rgb.at(x, y, 1) = g;
            rgb.at(x, y, 2) = b;
        }
    }
}

/// GBRG: top-left is G on B row.
void DebayerEngine::debayer_gbrg(const Image& raw, Image& rgb) {
    int w = raw.width();
    int h = raw.height();

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r, g, b;
            bool even_x = (x % 2 == 0);
            bool even_y = (y % 2 == 0);

            if (even_x && even_y) {
                // G on B row
                g = raw_at(raw, x, y);
                b = (raw_at(raw, x+1, y) + raw_at(raw, x-1, y)) * 0.5f;
                r = (raw_at(raw, x, y+1) + raw_at(raw, x, y-1)) * 0.5f;
            } else if (!even_x && even_y) {
                // B pixel
                b = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                r = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else if (even_x && !even_y) {
                // R pixel
                r = raw_at(raw, x, y);
                g = (raw_at(raw, x-1, y) + raw_at(raw, x+1, y) +
                     raw_at(raw, x, y-1) + raw_at(raw, x, y+1)) * 0.25f;
                b = (raw_at(raw, x-1, y-1) + raw_at(raw, x+1, y-1) +
                     raw_at(raw, x-1, y+1) + raw_at(raw, x+1, y+1)) * 0.25f;
            } else {
                // G on R row
                g = raw_at(raw, x, y);
                r = (raw_at(raw, x+1, y) + raw_at(raw, x-1, y)) * 0.5f;
                b = (raw_at(raw, x, y+1) + raw_at(raw, x, y-1)) * 0.5f;
            }

            rgb.at(x, y, 0) = r;
            rgb.at(x, y, 1) = g;
            rgb.at(x, y, 2) = b;
        }
    }
}

void DebayerEngine::suppress_banding(Image& rgb) {
    int w = rgb.width();
    int h = rgb.height();

    for (int ch = 0; ch < rgb.n_channels(); ch++) {
        // Work on a copy to avoid reading already-filtered values
        std::vector<float> orig(rgb.channel_data(ch),
                                rgb.channel_data(ch) + w * h);
        float* out = rgb.channel_data(ch);

        for (int y = 1; y < h - 1; y++) {
            for (int x = 1; x < w - 1; x++) {
                // Collect 3x3 neighborhood
                float vals[9];
                int k = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        vals[k++] = orig[(y + dy) * w + (x + dx)];

                // Partial sort to find median (5th element of 9)
                std::nth_element(vals, vals + 4, vals + 9);
                out[y * w + x] = vals[4];
            }
        }
    }
}

} // namespace nukex
