#include "nukex/core/running_accumulators.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/parallel_for.hpp"
#include "nukex/core/cancel_poll.hpp"

#include <stdexcept>

namespace nukex {

RunningAccumulators::RunningAccumulators(int width, int height, int n_channels)
    : width_(width), height_(height), n_channels_(n_channels)
{
    const std::size_t npx = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    static_assert(std::is_trivially_destructible_v<Channel>,
                  "Channel storage is released as raw bytes");
    // Constructed, never zeroed: WelfordAccumulator seeds min_val to +FLT_MAX
    // and max_val to -FLT_MAX, exactly as construct_voxel does. Default-init
    // `new std::byte[]` does not touch the memory; the rows are constructed
    // in place in parallel.
    const std::size_t nch = static_cast<std::size_t>(n_channels);
    channels_.reset(new std::byte[npx * nch * sizeof(Channel)]);
    const std::size_t row = static_cast<std::size_t>(width) * nch;
    std::byte* raw = channels_.get();
    parallel_for_dynamic(height, 8, [&](int y, int) {
        std::byte* p = raw + static_cast<std::size_t>(y) * row * sizeof(Channel);
        new (p) Channel[row]();
    });
    n_frames_.assign(npx, 0);
}

void RunningAccumulators::store_into(Cube& cube, CancelPoll* cancel) const {
    if (cube.width != width_ || cube.height != height_)
        throw std::invalid_argument("RunningAccumulators::store_into: geometry mismatch");
    if (cube.allocated_channels() < n_channels_)
        throw std::invalid_argument("RunningAccumulators::store_into: cube has fewer channels");
    const int w = width_, nc = n_channels_;
    // A DEFERRED record is constructed here, a row at a time, in the same
    // pass that fills it: one write of the file instead of two.
    const bool build = !cube.constructed();
    // A row per index: voxels of distinct rows are disjoint records, and a
    // whole row keeps each worker on contiguous pages of the record.
    auto row = [&](int y, int) {
        if (cancel && cancel->cancelled()) return;
        if (build) cube.construct_row(y);
        for (int x = 0; x < w; ++x) {
            if (cancel && (x & 1023) == 0 && cancel->cancelled()) return;
            const std::size_t p = static_cast<std::size_t>(y) * w + x;
            SubcubeVoxel& v = cube.at(x, y);
            for (int c = 0; c < nc; ++c) {
                const Channel& a = channel(p, c);
                v.channel(c).welford   = a.welford;
                v.channel(c).histogram = a.histogram;
            }
            v.n_frames = n_frames_[p];
        }
    };
    // With a CancelPoll the calling thread writes nothing and asks every
    // 20 ms until the workers are done; they read the flag every 1024 voxels.
    // Asking only from worker 0, between its own rows, left the tail of the
    // pass unwatched once it ran out of rows -- rows of a file-backed record
    // that page in from disk under memory pressure.
    if (cancel) parallel_for_polled(height_, 8, 0, row, [&] { cancel->check(); });
    else        parallel_for_dynamic(height_, 8, row);
    if (build && !(cancel && cancel->cancelled())) cube.mark_constructed();
}

void RunningAccumulators::release() {
    channels_.reset();
    std::vector<std::uint16_t>().swap(n_frames_);
    width_ = height_ = n_channels_ = 0;
}

} // namespace nukex
