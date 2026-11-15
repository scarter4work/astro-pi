#include "nukex/core/running_accumulators.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/parallel_for.hpp"

#include <stdexcept>

namespace nukex {

RunningAccumulators::RunningAccumulators(int width, int height, int n_channels)
    : width_(width), height_(height), n_channels_(n_channels)
{
    const std::size_t npx = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    // Value-constructed, never zeroed: WelfordAccumulator seeds min_val to
    // +FLT_MAX and max_val to -FLT_MAX, exactly as construct_voxel does.
    channels_.resize(npx * static_cast<std::size_t>(n_channels));
    n_frames_.assign(npx, 0);
}

void RunningAccumulators::store_into(Cube& cube) const {
    if (cube.width != width_ || cube.height != height_)
        throw std::invalid_argument("RunningAccumulators::store_into: geometry mismatch");
    if (cube.allocated_channels() < n_channels_)
        throw std::invalid_argument("RunningAccumulators::store_into: cube has fewer channels");
    const int w = width_, nc = n_channels_;
    // A row per index: voxels of distinct rows are disjoint records, and a
    // whole row keeps each worker on contiguous pages of the record.
    parallel_for_dynamic(height_, 8, [&](int y, int) {
        for (int x = 0; x < w; ++x) {
            const std::size_t p = static_cast<std::size_t>(y) * w + x;
            SubcubeVoxel& v = cube.at(x, y);
            for (int c = 0; c < nc; ++c) {
                const Channel& a = channel(p, c);
                v.channel(c).welford   = a.welford;
                v.channel(c).histogram = a.histogram;
            }
            v.n_frames = n_frames_[p];
        }
    });
}

void RunningAccumulators::release() {
    std::vector<Channel>().swap(channels_);
    std::vector<std::uint16_t>().swap(n_frames_);
    width_ = height_ = n_channels_ = 0;
}

} // namespace nukex
