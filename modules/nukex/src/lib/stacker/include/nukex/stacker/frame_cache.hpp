#pragma once

#include "nukex/io/image.hpp"
#include <string>
#include <cstdint>
#include <atomic>
#include <vector>
#include <mutex>

namespace nukex {

/// Disk-backed storage for aligned frames using memory-mapped uint16 encoding.
///
/// Pixel-major layout: read_pixel(x,y,ch) returns N contiguous uint16 values,
/// one per frame, for sequential disk reads during Phase B.
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

    /// Phase A: Write one aligned frame to the cache.
    /// Encodes float->uint16 and scatters to pixel-major positions.
    void write_frame(int frame_index, const Image& aligned);

    /// Phase B: Read all frame values at one pixel/channel, decoded to float.
    /// out_values must have space for at least n_frames_written() floats.
    /// Returns the highest-written-global-index + 1 (i.e. n_frames_written()),
    /// INCLUDING any unwritten interior gaps (which decode to 0). Kept for
    /// same-geometry callers where the written set is dense [0..n-1].
    int read_pixel(int x, int y, int ch, float* out_values) const;

    /// Phase B (per-channel accounting): read ONLY the frames actually written
    /// to this cache, densely packed in written-order into out_values. The
    /// k-th value corresponds to global frame index written_frames()[k].
    /// out_values must have space for at least written_frames().size() floats.
    /// Returns the number of real (written) frames = written_frames().size().
    ///
    /// This is the read path used for heterogeneous-geometry batches: it
    /// skips the phantom all-zero interior gaps that read_pixel() would
    /// return for a cache that only received a subset of the batch's global
    /// frame indices. For a same-geometry batch the written set is dense
    /// [0..n-1] and this is byte-identical to read_pixel().
    int read_pixel_dense(int x, int y, int ch, float* out_values) const;

    /// The GLOBAL frame indices actually written to this cache, in write
    /// order (deduplicated). For a same-geometry batch this is [0, 1, ..,
    /// n-1]; for a cache that only received a subset of a heterogeneous
    /// batch's frames it is exactly that subset. Local dense position k maps
    /// to global frame index written_frames()[k] — the map Phase B needs to
    /// index the flat per-frame frame_stats[] array correctly.
    const std::vector<int>& written_frames() const { return written_frames_; }

    /// Number of frames written so far. NOTE: this is (max written global
    /// index + 1), which for a heterogeneous batch OVER-reports the real
    /// sample count (it counts interior gaps). Use written_frames().size()
    /// for the true real-sample count.
    int n_frames_written() const { return n_frames_written_.load(std::memory_order_relaxed); }

    int width() const { return width_; }
    int height() const { return height_; }
    int n_channels() const { return n_channels_; }
    int max_frames() const { return max_frames_; }

    /// Encode float to uint16.
    static uint16_t encode(float value);

    /// Decode uint16 to float.
    static float decode(uint16_t stored);

private:
    int fd_ = -1;
    uint16_t* mapped_ = nullptr;
    size_t mapped_size_ = 0;
    std::string filepath_;

    int width_ = 0;
    int height_ = 0;
    int n_channels_ = 0;
    int max_frames_ = 0;
    std::atomic<int> n_frames_written_{0};

    /// Global frame indices actually written, in write order (deduplicated).
    /// Guarded by written_mutex_ because Phase A may write frames from
    /// multiple threads (append + dedup is not atomic on its own).
    std::vector<int> written_frames_;
    mutable std::mutex written_mutex_;

    /// Element offset for pixel (x, y), channel ch, frame f.
    size_t offset(int x, int y, int ch, int f) const {
        return static_cast<size_t>(
            ((static_cast<int64_t>(y) * width_ + x) * n_channels_ + ch)
            * max_frames_ + f);
    }

    void cleanup();
};

} // namespace nukex
