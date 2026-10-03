#pragma once

#include "nukex/core/types.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/voxel.hpp"
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>

namespace nukex {

/// The full-frame voxel record, one SubcubeVoxel per (x, y).
///
/// Records are runtime-sized: each is `voxel_record_size(n_channels)` bytes and
/// they are laid end to end in a single allocation, so a 1-channel stack costs
/// a fraction of what an 8-channel one does. The cube previously held a
/// std::vector<SubcubeVoxel> whose element carried MAX_CHANNELS worth of
/// per-channel arrays regardless of the stack.
class Cube {
public:
    int           width  = 0;
    int           height = 0;
    ChannelConfig channel_config;
    int           n_frames_loaded = 0;

    /// In-memory cube: one anonymous allocation of voxel_stride() * w * h.
    Cube(int w, int h, const ChannelConfig& config);

    /// File-backed cube: the same record store, mapped from a temporary file
    /// in `backing_dir`. Everything above this class is untouched -- it is a
    /// change of allocator, not of architecture.
    ///
    /// Why: at 604 B/voxel a 24 MP 4-channel stack is a 14.8 GB record, and
    /// as anonymous memory it is the reason a 30 GB machine goes into swap
    /// (PixInsight measured at 21 GB resident with 6 GB left). A clean
    /// file-backed page is simply dropped under pressure and re-read later --
    /// one I/O, often none -- where an anonymous page has to be written to
    /// swap first and read back: two. The file is unlinked the moment it is
    /// mapped, so a crash leaves nothing behind. Throws std::runtime_error
    /// when the file cannot be created or mapped; the caller falls back to
    /// memory and says so.
    Cube(int w, int h, const ChannelConfig& config, const std::string& backing_dir);

    Cube() = default;
    ~Cube();
    Cube(Cube&& other) noexcept;
    Cube& operator=(Cube&& other) noexcept;
    Cube(const Cube&) = delete;
    Cube& operator=(const Cube&) = delete;

    /// True when the record store is a file mapping rather than heap.
    bool file_backed() const { return fd_ >= 0; }

    SubcubeVoxel& at(int x, int y) {
        return *std::launder(reinterpret_cast<SubcubeVoxel*>(
            storage_ + offset_of(x, y)));
    }
    const SubcubeVoxel& at(int x, int y) const {
        return *std::launder(reinterpret_cast<const SubcubeVoxel*>(
            storage_ + offset_of(x, y)));
    }

    int total_pixels() const { return width * height; }

    /// Channels every voxel was allocated with. channel_config may be
    /// re-merged during Phase A; it must never exceed this, because the
    /// per-voxel records are sized against it and a larger slot index writes
    /// past the allocation.
    int allocated_channels() const { return allocated_channels_; }

    /// Bytes one voxel occupies, including its trailing channel records.
    std::size_t voxel_stride() const { return stride_; }

    /// Total bytes held by the voxel record. Worth logging before a stack:
    /// this is the number that decides whether the run fits in RAM.
    std::size_t bytes_allocated() const {
        return stride_ * static_cast<std::size_t>(width)
                       * static_cast<std::size_t>(height);
    }

    bool is_valid_coord(int x, int y) const {
        return x >= 0 && x < width && y >= 0 && y < height;
    }

private:
    std::size_t offset_of(int x, int y) const {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(x)) * stride_;
    }

    void construct_all();
    void release() noexcept;

    int          allocated_channels_ = 0;
    std::size_t  stride_   = 0;
    std::size_t  bytes_    = 0;
    std::byte*   storage_  = nullptr;
    int          fd_       = -1;      ///< backing file, or -1 for heap
};

} // namespace nukex
