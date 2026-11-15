#include "nukex/stacker/post_process.hpp"
#include "nukex/combine/output_assembler.hpp"
#include "nukex/core/parallel_for.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace nukex {

void compute_dominant_shape(SubcubeVoxel& voxel, int n_ch) {
    int counts[7] = {};
    for (int ch = 0; ch < n_ch; ch++) {
        int s = static_cast<int>(voxel.channel(ch).distribution.shape);
        if (s >= 0 && s < 7) counts[s]++;
    }
    int best = 0;
    for (int i = 1; i < 7; i++) {
        if (counts[i] > counts[best]) best = i;
    }
    voxel.dominant_shape = static_cast<DistributionShape>(best);
}

void post_process_record(Cube& cube, int n_ch,
                         const std::vector<float>& gradient_mag,
                         const std::vector<float>& local_background,
                         const std::vector<float>& local_rms,
                         Image& quality_map, Image& measured_noise) {
    const int w = cube.width, h = cube.height;
    const std::size_t npix = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    if (gradient_mag.size() != npix || local_background.size() != npix || local_rms.size() != npix)
        throw std::invalid_argument("post_process_record: spatial context does not match the cube");
    quality_map = Image(w, h, 4);
    measured_noise = Image(w, h, 1);

    parallel_for_dynamic(h, 4, [&](int y, int) {
        for (int x = 0; x < w; x++) {
            auto& voxel = cube.at(x, y);
            compute_dominant_shape(voxel, n_ch);

            float avg_snr = 0.0f;
            for (int ch = 0; ch < n_ch; ch++) avg_snr += voxel.channel(ch).snr;
            avg_snr /= n_ch;
            float cloud_fraction = (voxel.n_frames > 0) ?
                static_cast<float>(voxel.cloud_frame_count) / voxel.n_frames : 0.0f;
            voxel.quality_score = voxel.channel(0).distribution.confidence
                * (1.0f - cloud_fraction)
                * std::min(1.0f, avg_snr / 50.0f);
            voxel.confidence = voxel.channel(0).distribution.confidence;

            const std::size_t pi = static_cast<std::size_t>(y) * w + x;
            voxel.gradient_mag     = gradient_mag[pi];
            voxel.local_background = local_background[pi];
            voxel.local_rms        = local_rms[pi];

            float q[4];
            OutputAssembler::quality_pixel(voxel, n_ch, q);
            for (int c = 0; c < 4; c++) quality_map.at(x, y, c) = q[c];

            // Measured noise: the stack's realised pixel-to-pixel scatter,
            // beside the predicted noise_map. They answer different
            // questions, and until now only the prediction existed -- which
            // is why an estimator that adds noise of its own was invisible to
            // NukeX's own diagnostics.
            measured_noise.at(x, y, 0) = voxel.local_rms;
        }
    });
}

} // namespace nukex
