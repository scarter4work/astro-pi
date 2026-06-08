#pragma once

#include "nukex/io/image.hpp"
#include <string>
#include <cstdint>
#include <atomic>

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
    /// out_values must have space for at least n_frames_ floats.
    /// Returns number of frames written so far.
    int read_pixel(int x, int y, int ch, float* out_values) const;

    /// Number of frames written so far.
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

    /// Element offset for pixel (x, y), channel ch, frame f.
    size_t offset(int x, int y, int ch, int f) const {
        return static_cast<size_t>(
            ((static_cast<int64_t>(y) * width_ + x) * n_channels_ + ch)
            * max_frames_ + f);
    }

    void cleanup();
};

} // namespace nukex
