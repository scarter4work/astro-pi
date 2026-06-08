#include "test_data_loader.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/core/channel_config.hpp"
#include <filesystem>
#include <vector>
#include <algorithm>

namespace nukex { namespace test_util {

const std::string& m16_data_dir() {
    static const std::string dir = "/home/scarter4work/projects/processing/M16/";
    return dir;
}

Image load_m16_test_frame() {
    std::string dir = m16_data_dir();
    if (!std::filesystem::exists(dir)) return {};

    // Find first .fit file
    std::string path;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto ext = entry.path().extension().string();
        if (ext == ".fit" || ext == ".fits") {
            path = entry.path().string();
            break;
        }
    }
    if (path.empty()) return {};

    auto result = FITSReader::read(path);
    if (!result.success) return {};

    // Debayer if Bayer pattern detected
    if (!result.metadata.bayer_pattern.empty() &&
        result.metadata.bayer_pattern != "NONE") {
        // Parse bayer pattern
        BayerPattern bp = BayerPattern::RGGB;  // Default for ZWO cameras
        if (result.metadata.bayer_pattern == "BGGR") bp = BayerPattern::BGGR;
        else if (result.metadata.bayer_pattern == "GRBG") bp = BayerPattern::GRBG;
        else if (result.metadata.bayer_pattern == "GBRG") bp = BayerPattern::GBRG;

        // Equalize Bayer sub-channel backgrounds to remove checkerboard banding
        DebayerEngine::equalize_bayer_background(result.image, bp);

        Image rgb = DebayerEngine::debayer(result.image, bp);

        // Suppress bilinear debayer banding artifacts
        DebayerEngine::suppress_banding(rgb);

        return rgb;
    }

    return std::move(result.image);
}

void prepare_for_stretch(Image& img, float clip_percentile) {
    int npix = img.width() * img.height();

    for (int c = 0; c < img.n_channels(); c++) {
        float* data = img.channel_data(c);

        // Compute median (background estimate)
        std::vector<float> sorted(data, data + npix);
        std::nth_element(sorted.begin(), sorted.begin() + npix / 2, sorted.end());
        float median = sorted[npix / 2];

        // Compute clip level (reject hot pixels)
        size_t clip_idx = static_cast<size_t>(npix * clip_percentile);
        clip_idx = std::min(clip_idx, static_cast<size_t>(npix - 1));
        std::nth_element(sorted.begin(), sorted.begin() + clip_idx, sorted.end());
        float clip = sorted[clip_idx];

        float range = clip - median;
        if (range < 1e-10f) continue;

        float inv_range = 1.0f / range;
        for (int i = 0; i < npix; i++) {
            data[i] = std::clamp((data[i] - median) * inv_range, 0.0f, 1.0f);
        }
    }
}

}} // namespace nukex::test_util
