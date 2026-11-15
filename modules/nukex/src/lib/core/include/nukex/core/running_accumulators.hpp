#pragma once

#include "nukex/core/histogram.hpp"
#include "nukex/core/welford.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <vector>

namespace nukex {

class Cube;
class CancelPoll;

/// Fold one sample into a channel's running accumulators.
///
/// The single definition of what Phase A records per sample, shared by both
/// places those accumulators can live (the voxel record and
/// RunningAccumulators), so the two cannot drift apart. An uncovered sample
/// is an absence, not a measurement of darkness: warp leaves 0 outside the
/// source and 0 is a legal value, so it is skipped rather than averaged in.
inline void accumulate_sample(WelfordAccumulator& welford, PixelHistogram& histogram,
                              float value, bool covered) {
    if (!covered) return;
    welford.update(value);
    if (welford.count() == 1)
        histogram.initialize_range(value - 0.1f, value + 0.1f);
    histogram.update(value);
}

/// Phase A's running per-voxel accumulators, held apart from the voxel record.
///
/// Phase A updates a channel's Welford state and histogram once per frame for
/// every pixel. When the voxel record is a file mapping (it is 14.8 GB for a
/// 24 MP 4-channel stack, more than a 30 GB machine can keep resident beside
/// the frame cache), those few bytes per channel sit inside 604-byte records,
/// so every frame dirtied every page of the file and the kernel wrote the
/// whole record back: 953 GB written to the NVMe for a 59-frame stack, and
/// 696 s of its 1062 s.
///
/// Here they are one dense heap array of exactly the Phase A fields
/// (n_ch * 60 + 2 bytes per pixel, 5.9 GB at 24 MP x 4 channels), updated in
/// place in RAM, and copied into the record ONCE by store_into() when Phase A
/// ends. The update sequence each voxel sees is unchanged, so the record is
/// bit-identical to accumulating into it directly.
class RunningAccumulators {
public:
    struct Channel {
        WelfordAccumulator welford;
        PixelHistogram     histogram;
    };

    RunningAccumulators() = default;
    /// Throws std::bad_alloc when the array cannot be allocated.
    RunningAccumulators(int width, int height, int n_channels);

    /// Heap bytes this many pixels and channels need.
    static std::size_t bytes_for(int width, int height, int n_channels) {
        return static_cast<std::size_t>(width) * static_cast<std::size_t>(height)
             * (static_cast<std::size_t>(n_channels) * sizeof(Channel) + sizeof(std::uint16_t));
    }
    std::size_t bytes_allocated() const { return bytes_for(width_, height_, n_channels_); }

    int width() const      { return width_; }
    int height() const     { return height_; }
    int n_channels() const { return n_channels_; }
    bool empty() const     { return !channels_; }

    /// Channel `c` of pixel `pixel` (raster index y * width + x).
    Channel& channel(std::size_t pixel, int c) {
        return base()[pixel * static_cast<std::size_t>(n_channels_) + static_cast<std::size_t>(c)];
    }
    const Channel& channel(std::size_t pixel, int c) const {
        return const_cast<RunningAccumulators*>(this)->channel(pixel, c);
    }
    std::uint16_t& n_frames(std::size_t pixel)       { return n_frames_[pixel]; }
    std::uint16_t  n_frames(std::size_t pixel) const { return n_frames_[pixel]; }

    /// Copy every accumulator into the matching voxel of `cube`: its welford,
    /// histogram and n_frames. Pixel-parallel, one pass over the record.
    /// The cube must have this geometry and at least this many channels.
    /// With `cancel`, rows stop being copied once it reads cancelled; the
    /// record is then incomplete and the caller must abandon it.
    void store_into(Cube& cube, CancelPoll* cancel = nullptr) const;

    /// Release the array (after store_into, before Phase B allocates).
    void release();

private:
    Channel* base() { return std::launder(reinterpret_cast<Channel*>(channels_.get())); }

    int width_ = 0, height_ = 0, n_channels_ = 0;
    // Raw storage, constructed in place in parallel (a std::vector would
    // value-initialise 5.9 GB on one thread: 1.8 s at 24 MP x 4 channels).
    // Channel is trivially destructible, so releasing the bytes is enough.
    std::unique_ptr<std::byte[]> channels_;
    std::vector<std::uint16_t> n_frames_;
};

} // namespace nukex
