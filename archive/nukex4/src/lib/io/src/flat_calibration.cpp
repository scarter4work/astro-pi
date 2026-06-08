#include "nukex/io/flat_calibration.hpp"
#include "nukex/io/fits_reader.hpp"
#include <algorithm>
#include <stdexcept>
#include <numeric>

namespace nukex {

float FlatCalibration::median(const Image& img) {
    std::vector<float> values(img.data(), img.data() + img.data_size());
    size_t n = values.size();
    if (n == 0) return 0.0f;

    std::nth_element(values.begin(), values.begin() + n / 2, values.end());
    float med = values[n / 2];

    if (n % 2 == 0) {
        float lower = *std::max_element(values.begin(), values.begin() + n / 2);
        med = (med + lower) * 0.5f;
    }

    return med;
}

Image FlatCalibration::build_master_flat(const std::vector<std::string>& flat_paths) {
    if (flat_paths.empty()) {
        throw std::invalid_argument("FlatCalibration: no flat frames provided");
    }

    // Read all flats
    std::vector<Image> flats;
    flats.reserve(flat_paths.size());

    for (const auto& path : flat_paths) {
        auto result = FITSReader::read(path);
        if (!result.success) {
            throw std::runtime_error("FlatCalibration: failed to read " + path +
                                     ": " + result.error);
        }
        // Normalize each flat by its own median
        float med = median(result.image);
        if (med > 1e-10f) {
            float scale = 1.0f / med;
            result.image.apply([scale](float x) { return x * scale; });
        }
        flats.push_back(std::move(result.image));
    }

    // Verify all flats have the same dimensions
    int w = flats[0].width();
    int h = flats[0].height();
    int nc = flats[0].n_channels();
    for (size_t i = 1; i < flats.size(); i++) {
        if (flats[i].width() != w || flats[i].height() != h ||
            flats[i].n_channels() != nc) {
            throw std::runtime_error("FlatCalibration: flat frame dimension mismatch");
        }
    }

    // Median combine: for each pixel, take the median across all flats
    Image master(w, h, nc);
    size_t n_flats = flats.size();
    std::vector<float> pixel_values(n_flats);

    for (int ch = 0; ch < nc; ch++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                for (size_t f = 0; f < n_flats; f++) {
                    pixel_values[f] = flats[f].at(x, y, ch);
                }
                std::nth_element(pixel_values.begin(),
                                 pixel_values.begin() + n_flats / 2,
                                 pixel_values.end());
                float med = pixel_values[n_flats / 2];
                // For even N, average the two central values (consistent
                // with FlatCalibration::median).
                if (n_flats % 2 == 0) {
                    float lower = *std::max_element(
                        pixel_values.begin(),
                        pixel_values.begin() + n_flats / 2);
                    med = (med + lower) * 0.5f;
                }
                master.at(x, y, ch) = med;
            }
        }
    }

    return master;
}

void FlatCalibration::apply(Image& light, const Image& master_flat,
                            float min_flat_value) {
    if (light.width() != master_flat.width() ||
        light.height() != master_flat.height()) {
        throw std::invalid_argument("FlatCalibration::apply: dimension mismatch");
    }

    // For single-channel master flat applied to multi-channel light:
    // divide each channel by the same flat (luminance flat).
    // For matching channel counts: divide channel by channel.
    // Any other combination is a user error — throw rather than silently
    // falling back to channel 0.
    int flat_channels = master_flat.n_channels();
    int light_channels = light.n_channels();

    if (flat_channels != 1 && flat_channels != light_channels) {
        throw std::invalid_argument(
            "FlatCalibration::apply: flat has " + std::to_string(flat_channels) +
            " channels but light has " + std::to_string(light_channels) +
            " channels (expected 1 or matching count)");
    }

    for (int ch = 0; ch < light_channels; ch++) {
        int flat_ch = (flat_channels == 1) ? 0 : ch;

        for (int y = 0; y < light.height(); y++) {
            for (int x = 0; x < light.width(); x++) {
                float flat_val = master_flat.at(x, y, flat_ch);
                // Clamp flat to minimum to avoid amplifying noise in vignetted areas
                if (flat_val < min_flat_value) flat_val = min_flat_value;
                light.at(x, y, ch) /= flat_val;
            }
        }
    }
}

} // namespace nukex
