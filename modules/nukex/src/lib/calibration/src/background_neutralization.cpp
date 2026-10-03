#include "nukex/calibration/background_neutralization.hpp"

#include <algorithm>
#include <vector>

namespace nukex {

namespace {

/// Median of a plane, sampled on a stride. Matches the estimator
/// VeraLuxStretch::auto_tune solves its shadow point against, so the two
/// steps agree about where the background is.
float plane_median(const float* p, std::size_t n) {
    const std::size_t stride = std::max<std::size_t>(1, n / 200000);
    std::vector<float> sample;
    sample.reserve(n / stride + 1);
    for (std::size_t i = 0; i < n; i += stride) sample.push_back(p[i]);
    if (sample.empty()) return 0.0f;
    const std::size_t mid = sample.size() / 2;
    std::nth_element(sample.begin(), sample.begin() + mid, sample.end());
    return sample[mid];
}

} // namespace

BackgroundOffsets match_plane_backgrounds(const std::vector<float*>& planes,
                                          std::size_t n_pixels) {
    BackgroundOffsets out;
    if (planes.size() < 2 || n_pixels == 0) return out;

    std::vector<float> medians;
    medians.reserve(planes.size());
    for (const float* p : planes) medians.push_back(plane_median(p, n_pixels));

    const float ref = *std::min_element(medians.begin(), medians.end());

    out.subtracted.reserve(planes.size());
    for (std::size_t c = 0; c < planes.size(); ++c) {
        const float offset = medians[c] - ref;
        out.subtracted.push_back(offset);
        if (offset <= 0.0f) continue;
        out.applied = true;
        float* p = planes[c];
        for (std::size_t i = 0; i < n_pixels; ++i)
            p[i] = std::max(0.0f, p[i] - offset);
    }
    return out;
}

std::vector<int> chroma_slot_indices(const ChannelConfig& cfg) {
    std::vector<int> out;
    for (int i = 0; i < static_cast<int>(cfg.n_channels); ++i) {
        const std::string& n = cfg.channel_names[i];
        if (n == "R" || n == "G" || n == "B") out.push_back(i);
    }
    return out;
}

BackgroundOffsets neutralize_chroma_slots(Image& img, const ChannelConfig& cfg) {
    const std::vector<int> idx = chroma_slot_indices(cfg);
    const std::size_t n = static_cast<std::size_t>(img.width()) * img.height();
    if (idx.size() < 2 || n == 0) return BackgroundOffsets{};

    std::vector<float*> planes;
    planes.reserve(idx.size());
    for (int i : idx) {
        if (i >= img.n_channels()) return BackgroundOffsets{};
        planes.push_back(img.channel_data(i));
    }
    return match_plane_backgrounds(planes, n);
}

BackgroundOffsets neutralize_channel_backgrounds(Image& img) {
    const int nch = img.n_channels();
    const std::size_t n = static_cast<std::size_t>(img.width()) * img.height();
    if (nch < 2 || n == 0) return BackgroundOffsets{};

    std::vector<float*> planes;
    planes.reserve(static_cast<std::size_t>(nch));
    for (int c = 0; c < nch; ++c) planes.push_back(img.channel_data(c));
    return match_plane_backgrounds(planes, n);
}

} // namespace nukex
