#include "nukex/stretch/stretch_utils.hpp"
#include <cmath>
#include <algorithm>

namespace nukex {

void apply_luminance_only(Image& img, const std::function<float(float)>& fn) {
    int w = img.width(), h = img.height(), nch = img.n_channels();

    if (nch == 1) {
        // Single channel: apply directly
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                img.at(x, y, 0) = fn(img.at(x, y, 0));
        return;
    }

    // Multi-channel: luminance-preserving stretch
    // Rec. 709 luminance coefficients
    constexpr float kR = 0.2126f, kG = 0.7152f, kB = 0.0722f;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float r = img.at(x, y, 0);
            float g = (nch > 1) ? img.at(x, y, 1) : r;
            float b = (nch > 2) ? img.at(x, y, 2) : r;

            float L = kR * r + kG * g + kB * b;
            if (L < 1e-10f) continue;  // Black pixel — nothing to scale

            float L_stretched = fn(L);
            float scale = L_stretched / L;

            img.at(x, y, 0) = std::clamp(r * scale, 0.0f, 1.0f);
            if (nch > 1) img.at(x, y, 1) = std::clamp(g * scale, 0.0f, 1.0f);
            if (nch > 2) img.at(x, y, 2) = std::clamp(b * scale, 0.0f, 1.0f);
        }
    }
}

void apply_per_channel(Image& img, const std::function<float(float)>& fn) {
    for (int ch = 0; ch < img.n_channels(); ch++) {
        float* data = img.channel_data(ch);
        int n = img.width() * img.height();
        for (int i = 0; i < n; i++) {
            data[i] = fn(data[i]);
        }
    }
}

void clamp_image(Image& img, float lo, float hi) {
    float* data = img.data();
    size_t n = img.data_size();
    for (size_t i = 0; i < n; i++) {
        data[i] = std::clamp(data[i], lo, hi);
    }
}

} // namespace nukex
