#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/coverage_mask.hpp"
#include <string>
#include <cstdint>
#include <atomic>
#include <vector>

namespace nukex {

/// Disk-backed storage for aligned frames using memory-mapped uint16 encoding.
///
/// Frame-major layout: one frame's pixels are consecutive, so Phase A writes
/// each frame as a single sequential run and touches nothing else. The
/// previous pixel-major index put consecutive writes for one frame
/// max_frames elements apart, which meant writing a 66 MB frame dirtied every
/// page of a 10.4 GB mapping -- 26x write amplification measured on a
/// 156-frame 24 MP run, and the reason Phase A's per-frame cost climbed from
/// 3.85 s to 13.4 s as the cache filled.
///
/// Phase B reads it back a frame-row at a time (read_frame_range), which is
/// how it already batches: n_frames contiguous runs per batch rather than one
/// gather per pixel.
///
/// Encoding: float [0,1] -> uint16 via round(value * 65535)
/// Decoding: uint16 -> float via stored * (1.0f / 65535.0f)
/// Quantization error: +/-7.6e-6 (100x below noise floor).
///
/// The temp file is deleted when the FrameCache is destroyed.
class FrameCache {
public:
    /// Create a cache file in cache_dir. Pre-allocates for max_frames frames.
    FrameCache(int width, int height, int n_channels,
               int max_frames, const std::string& cache_dir);

    /// Unmaps and deletes the temp file.
    ~FrameCache();

    // Non-copyable, movable
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;
    FrameCache(FrameCache&& other) noexcept;
    FrameCache& operator=(FrameCache&& other) noexcept;

    /// Phase A: Append one aligned frame at the next LOCAL slot.
    ///
    /// Returns the local slot it was written to. `global_index` is the
    /// frame's number within the whole batch, recorded so Phase B can
    /// recover its FrameStats -- those are indexed globally, while every
    /// pixel read here is indexed locally.
    ///
    /// Local slots are dense by construction. They used to be the global
    /// index, which made that count wrong for any cache that received a
    /// subset of the batch: a cache given frames 5, 9 and 12 of twenty
    /// reported n_frames_written() == 13, and Phase B trusts that count to
    /// bound which local slots read_frame_range treats as written -- it
    /// would have read ten unwritten slots as though they were measurements.
    /// `coverage` says which pixels of `aligned` actually carry source data.
    /// An empty mask means the frame was cloned rather than warped and covers
    /// itself completely.
    ///
    /// Coverage has to live in the cache, not only in the Phase A
    /// accumulators: `stacked` is produced by Phase B, which reads its
    /// per-frame samples back from here. Storing the warped zeros without
    /// saying they are absent is what left a rim at 94% of interior
    /// brightness even after the accumulators were guarded.
    int write_frame(const Image& aligned, int global_index,
                    const CoverageMask& coverage = CoverageMask{});

    /// The batch-global frame number stored at `local`. -1 if out of range.
    int global_frame(int local) const {
        return (local >= 0 && local < static_cast<int>(frame_map_.size()))
             ? frame_map_[local] : -1;
    }

    /// Phase B: read one frame slot's channel over a contiguous pixel run.
    ///
    /// Pixels are addressed in raster order: `start_pixel` is `y*width + x`,
    /// and `out_values[i]` receives pixel `start_pixel + i` of frame slot `f`.
    /// `out_valid` may be null; when given it reports, per pixel, whether that
    /// frame actually covered it.
    ///
    /// This is the granularity Phase B batches at, and the granularity the
    /// storage layout is built for. Reading one pixel across all frames --
    /// what read_pixel did -- is the access pattern a frame-major layout is
    /// worst at, and it is why that method no longer exists.
    ///
    /// Returns false and writes NOTHING if `f` is not a written slot or the
    /// range leaves the image. Refusing totally rather than partially matters:
    /// a caller handed half a row would fit a distribution to whatever was
    /// left in its buffer.
    bool read_frame_range(int f, int start_pixel, int count, int ch,
                          float* out_values,
                          std::uint8_t* out_valid = nullptr) const;

    /// Number of frames written so far.
    int n_frames_written() const { return n_frames_written_.load(std::memory_order_relaxed); }

    int width() const { return width_; }
    int height() const { return height_; }
    int n_channels() const { return n_channels_; }
    int max_frames() const { return max_frames_; }

    /// Schedule writeback of everything written so far.
    ///
    /// Call at the end of Phase A. Not needed for correctness -- an mmap is
    /// coherent within the process, so Phase B sees every write regardless --
    /// only to bound how many dirty pages the machine is holding.
    void flush();

    /// How many times writeback has been scheduled -- one per cached frame
    /// plus any explicit flush(). Diagnostic: with a frame-major layout each
    /// sync covers only that frame's own bytes, so this number multiplied by
    /// one frame's size is roughly the volume written.
    int sync_count() const { return sync_count_; }

    /// Encode float to uint16.
    static uint16_t encode(float value);

    /// Decode uint16 to float.
    static float decode(uint16_t stored);

    /// Element index of (x, y, ch) in frame slot f.
    ///
    /// Public and static so the layout can be asserted directly rather than
    /// inferred from read-back values -- the storage order is the whole point
    /// of this class, and it should not be possible to change it silently.
    ///
    /// Note what is ABSENT: max_frames. The old index multiplied by it, which
    /// is why one frame's writes were spread across the entire mapping.
    static std::size_t element_offset(int width, int height, int n_channels,
                                      int x, int y, int ch, int f) {
        const std::size_t frame_elems =
            static_cast<std::size_t>(width) * height * n_channels;
        return static_cast<std::size_t>(f) * frame_elems
             + (static_cast<std::size_t>(y) * width + x) * n_channels
             + static_cast<std::size_t>(ch);
    }

private:
    int fd_ = -1;
    uint16_t* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    /// Coverage bitplane, one bit per (pixel, channel, frame), laid out with
    /// the same index as offset(). Lives after the value region in the same
    /// mapping.
    std::uint8_t* coverage_bits_ = nullptr;
    size_t coverage_bytes_ = 0;
    std::string filepath_;

    int width_ = 0;
    int height_ = 0;
    int n_channels_ = 0;
    int max_frames_ = 0;
    int sync_count_ = 0;
    std::atomic<int> n_frames_written_{0};
    std::vector<int> frame_map_;   ///< local slot -> batch-global frame index

    /// Element offset for pixel (x, y), channel ch, frame f.
    size_t offset(int x, int y, int ch, int f) const {
        return element_offset(width_, height_, n_channels_, x, y, ch, f);
    }

    /// Bit index of (x, y, ch, f) in the coverage plane -- same ordering as
    /// offset(), so the two stay in step by construction.
    size_t cov_bit(int x, int y, int ch, int f) const { return offset(x, y, ch, f); }

    /// Schedule writeback of one byte range of the mapping.
    ///
    /// msync needs a page-aligned address, so the range is widened outward to
    /// page boundaries -- writing back a few neighbouring pages is free
    /// compared with the alternative of writing back all of them.
    void sync_range(std::size_t byte_begin, std::size_t byte_end);

    void cleanup();
};

} // namespace nukex
