#include "nukex/stacker/frame_cache.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <stdexcept>

static_assert(sizeof(off_t) >= 8, "off_t must be 64-bit for large file support");

namespace nukex {

uint16_t FrameCache::encode(float value) {
    float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<uint16_t>(clamped * 65535.0f + 0.5f);
}

float FrameCache::decode(uint16_t stored) {
    return stored * (1.0f / 65535.0f);
}

FrameCache::FrameCache(int width, int height, int n_channels,
                       int max_frames, const std::string& cache_dir)
    : width_(width), height_(height), n_channels_(n_channels),
      max_frames_(max_frames)
{
    // Compute total size
    size_t n_entries = static_cast<size_t>(width) * height * n_channels * max_frames;
    mapped_size_ = n_entries * sizeof(uint16_t);

    // Create temp file
    filepath_ = cache_dir + "/nukex_cache_XXXXXX";
    // mkstemp needs a mutable char array
    std::vector<char> path_buf(filepath_.begin(), filepath_.end());
    path_buf.push_back('\0');
    fd_ = mkstemp(path_buf.data());
    if (fd_ < 0) {
        throw std::runtime_error("FrameCache: failed to create temp file in " + cache_dir);
    }
    filepath_ = std::string(path_buf.data());

    // Extend file to full size
    if (ftruncate(fd_, static_cast<off_t>(mapped_size_)) != 0) {
        cleanup();
        throw std::runtime_error("FrameCache: ftruncate failed");
    }

    // Memory map
    mapped_ = static_cast<uint16_t*>(
        mmap(nullptr, mapped_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;
        cleanup();
        throw std::runtime_error("FrameCache: mmap failed");
    }
}

FrameCache::~FrameCache() {
    cleanup();
}

void FrameCache::cleanup() {
    if (mapped_ && mapped_ != MAP_FAILED) {
        munmap(mapped_, mapped_size_);
        mapped_ = nullptr;
    }
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    if (!filepath_.empty()) {
        unlink(filepath_.c_str());
        filepath_.clear();
    }
}

FrameCache::FrameCache(FrameCache&& other) noexcept
    : fd_(other.fd_), mapped_(other.mapped_), mapped_size_(other.mapped_size_),
      filepath_(std::move(other.filepath_)),
      width_(other.width_), height_(other.height_),
      n_channels_(other.n_channels_), max_frames_(other.max_frames_),
      n_frames_written_(other.n_frames_written_.load(std::memory_order_relaxed))
{
    other.fd_ = -1;
    other.mapped_ = nullptr;
    other.mapped_size_ = 0;
}

FrameCache& FrameCache::operator=(FrameCache&& other) noexcept {
    if (this != &other) {
        cleanup();
        fd_ = other.fd_;
        mapped_ = other.mapped_;
        mapped_size_ = other.mapped_size_;
        filepath_ = std::move(other.filepath_);
        width_ = other.width_;
        height_ = other.height_;
        n_channels_ = other.n_channels_;
        max_frames_ = other.max_frames_;
        n_frames_written_.store(other.n_frames_written_.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        other.fd_ = -1;
        other.mapped_ = nullptr;
        other.mapped_size_ = 0;
    }
    return *this;
}

void FrameCache::write_frame(int frame_index, const Image& aligned) {
    if (!mapped_) throw std::runtime_error("FrameCache: not mapped");
    if (frame_index < 0 || frame_index >= max_frames_)
        throw std::out_of_range("FrameCache: frame_index out of range");
    if (aligned.width() != width_ || aligned.height() != height_ || aligned.n_channels() != n_channels_)
        throw std::invalid_argument("FrameCache::write_frame: image dimensions do not match cache");

    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            for (int ch = 0; ch < n_channels_; ch++) {
                float value = aligned.at(x, y, ch);
                mapped_[offset(x, y, ch, frame_index)] = encode(value);
            }
        }
    }

    int current = n_frames_written_.load(std::memory_order_relaxed);
    if (frame_index >= current) {
        n_frames_written_.store(frame_index + 1, std::memory_order_relaxed);
    }
}

int FrameCache::read_pixel(int x, int y, int ch, float* out_values) const {
    if (!mapped_) return 0;

    int n = n_frames_written_.load(std::memory_order_relaxed);
    size_t base = offset(x, y, ch, 0);

    // Contiguous uint16 values for this pixel/channel across all frames
    for (int f = 0; f < n; f++) {
        out_values[f] = decode(mapped_[base + f]);
    }

    return n;
}

} // namespace nukex
