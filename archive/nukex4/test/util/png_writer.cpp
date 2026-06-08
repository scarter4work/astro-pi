#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "png_writer.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

namespace nukex { namespace test_util {

// Convert [0,1] float image to big-endian 16-bit and write via stb
static bool write_16bit_png(const std::string& filepath,
                            const std::vector<float>& buf,
                            int w, int h, int ch) {
    std::vector<uint16_t> pixels(w * h * ch);
    for (size_t i = 0; i < buf.size(); i++) {
        float val = std::clamp(buf[i], 0.0f, 1.0f);
        uint16_t u16 = static_cast<uint16_t>(val * 65535.0f + 0.5f);
        // stb_image_write expects big-endian for 16-bit PNG
        pixels[i] = (u16 >> 8) | (u16 << 8);
    }
    int stride = w * ch * 2;
    return stbi_write_png(filepath.c_str(), w, h, ch, pixels.data(), stride) != 0;
}

bool write_png(const std::string& filepath, const nukex::Image& img,
               bool apply_stretch, float stretch_alpha) {
    if (img.empty()) return false;

    int w = img.width();
    int h = img.height();
    int ch = std::min(img.n_channels(), 3);

    std::vector<float> buf(w * h * ch);
    float norm = (stretch_alpha > 0.0f) ? std::asinh(stretch_alpha) : 1.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < ch; c++) {
                float val = img.at(x, y, c);
                if (apply_stretch && stretch_alpha > 0.0f) {
                    val = std::asinh(stretch_alpha * val) / norm;
                }
                buf[(y * w + x) * ch + c] = val;
            }
        }
    }

    return write_16bit_png(filepath, buf, w, h, ch);
}

bool write_png_8bit(const std::string& filepath, const nukex::Image& img,
                    bool apply_stretch, float stretch_alpha) {
    if (img.empty()) return false;

    int w = img.width();
    int h = img.height();
    int ch = std::min(img.n_channels(), 3);

    std::vector<uint8_t> pixels(w * h * ch);
    float norm = (stretch_alpha > 0.0f) ? std::asinh(stretch_alpha) : 1.0f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < ch; c++) {
                float val = img.at(x, y, c);
                if (apply_stretch && stretch_alpha > 0.0f) {
                    val = std::asinh(stretch_alpha * val) / norm;
                }
                val = std::clamp(val, 0.0f, 1.0f);
                pixels[(y * w + x) * ch + c] = static_cast<uint8_t>(val * 255.0f + 0.5f);
            }
        }
    }

    int stride = w * ch;
    return stbi_write_png(filepath.c_str(), w, h, ch, pixels.data(), stride) != 0;
}

bool write_png_auto(const std::string& filepath, const nukex::Image& img,
                    float clip_percentile, float stretch_alpha) {
    if (img.empty()) return false;

    int w = img.width();
    int h = img.height();
    int ch = std::min(img.n_channels(), 3);
    int npix = w * h;

    // Per-channel background subtraction and percentile clipping
    // This mimics PixInsight's STF auto-stretch approach:
    //   1. Estimate background as median
    //   2. Clip at a high percentile to reject hot pixels / saturated stars
    //   3. Normalize signal to [0, 1]
    //   4. Apply arcsinh stretch

    // Compute per-channel median and clip level
    struct ChannelStats { float median; float clip; };
    std::vector<ChannelStats> stats(ch);

    for (int c = 0; c < ch; c++) {
        std::vector<float> sorted(npix);
        for (int i = 0; i < npix; i++) {
            sorted[i] = img.channel_data(std::min(c, img.n_channels() - 1))[i];
        }
        size_t mid = npix / 2;
        std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.end());
        stats[c].median = sorted[mid];

        size_t clip_idx = static_cast<size_t>(npix * clip_percentile);
        clip_idx = std::min(clip_idx, static_cast<size_t>(npix - 1));
        std::nth_element(sorted.begin(), sorted.begin() + clip_idx, sorted.end());
        stats[c].clip = sorted[clip_idx];
    }

    float norm = (stretch_alpha > 0.0f) ? std::asinh(stretch_alpha) : 1.0f;

    std::vector<float> buf(w * h * ch);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            for (int c = 0; c < ch; c++) {
                float val = img.at(x, y, c);

                // Subtract background
                val -= stats[c].median;
                if (val < 0.0f) val = 0.0f;

                // Normalize by clip level (signal fills [0, 1])
                float range = stats[c].clip - stats[c].median;
                if (range > 1e-10f) {
                    val /= range;
                }
                if (val > 1.0f) val = 1.0f;

                // Apply arcsinh stretch
                if (stretch_alpha > 0.0f) {
                    val = std::asinh(stretch_alpha * val) / norm;
                }

                buf[(y * w + x) * ch + c] = val;
            }
        }
    }

    return write_16bit_png(filepath, buf, w, h, ch);
}

}} // namespace nukex::test_util
