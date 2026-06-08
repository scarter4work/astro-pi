#include "nukex/combine/output_assembler.hpp"
#include <algorithm>

namespace nukex {

Image OutputAssembler::assemble_quality_map(const Cube& cube) {
    Image quality(cube.width, cube.height, 4);
    int n_ch = cube.channel_config.n_channels;

    for (int y = 0; y < cube.height; y++) {
        for (int x = 0; x < cube.width; x++) {
            const auto& v = cube.at(x, y);

            float avg_signal = 0.0f;
            float max_uncertainty = 0.0f;
            float min_confidence = 1.0f;

            for (int ch = 0; ch < n_ch; ch++) {
                const auto& dist = v.distribution[ch];
                avg_signal += dist.true_signal_estimate;
                max_uncertainty = std::max(max_uncertainty, dist.signal_uncertainty);
                min_confidence = std::min(min_confidence, dist.confidence);
            }
            avg_signal /= n_ch;

            quality.at(x, y, 0) = avg_signal;
            quality.at(x, y, 1) = max_uncertainty;
            quality.at(x, y, 2) = min_confidence;
            quality.at(x, y, 3) = static_cast<float>(
                static_cast<uint8_t>(v.dominant_shape));
        }
    }

    return quality;
}

} // namespace nukex
