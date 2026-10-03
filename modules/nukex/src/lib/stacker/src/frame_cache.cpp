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
    // Values, then a coverage bitplane of one bit per entry. The bitplane
    // costs 1/16th of the value region, which is what it takes for Phase B to
    // tell an absent sample from a measured zero.
    coverage_bytes_ = (n_entries + 7) / 8;
    mapped_size_ = n_entries * sizeof(uint16_t) + coverage_bytes_;

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
    coverage_bits_ = reinterpret_cast<std::uint8_t*>(mapped_) + n_entries * sizeof(uint16_t);
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

void FrameCache::sync_range(std::size_t byte_begin, std::size_t byte_end) {
    if (!mapped_ || byte_end <= byte_begin) return;
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const std::size_t begin = byte_begin & ~(page - 1);
    std::size_t end = (byte_end + page - 1) & ~(page - 1);
    if (end > mapped_size_) end = mapped_size_;
    if (end <= begin) return;
    // MS_ASYNC only schedules it, so this never stalls the caching loop.
    msync(reinterpret_cast<char*>(mapped_) + begin, end - begin, MS_ASYNC);
}

FrameCache::FrameCache(FrameCache&& other) noexcept
    : fd_(other.fd_), mapped_(other.mapped_), mapped_size_(other.mapped_size_),
      coverage_bits_(other.coverage_bits_),
      coverage_bytes_(other.coverage_bytes_),
      filepath_(std::move(other.filepath_)),
      width_(other.width_), height_(other.height_),
      n_channels_(other.n_channels_), max_frames_(other.max_frames_),
      sync_count_(other.sync_count_),
      n_frames_written_(other.n_frames_written_.load(std::memory_order_relaxed)),
      frame_map_(std::move(other.frame_map_))
{
    other.fd_ = -1;
    other.coverage_bits_ = nullptr;
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
        sync_count_ = other.sync_count_;
        n_frames_written_.store(other.n_frames_written_.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        frame_map_ = std::move(other.frame_map_);
        coverage_bits_ = other.coverage_bits_;
        coverage_bytes_ = other.coverage_bytes_;
        other.coverage_bits_ = nullptr;
        other.fd_ = -1;
        other.mapped_ = nullptr;
        other.mapped_size_ = 0;
    }
    return *this;
}

int FrameCache::write_frame(const Image& aligned, int global_index,
                            const CoverageMask& coverage) {
    if (!mapped_) throw std::runtime_error("FrameCache: not mapped");
    const int frame_index = static_cast<int>(frame_map_.size());
    if (frame_index >= max_frames_)
        throw std::out_of_range("FrameCache: more frames than the cache was sized for");
    if (aligned.width() != width_ || aligned.height() != height_ || aligned.n_channels() != n_channels_)
        throw std::invalid_argument("FrameCache::write_frame: image dimensions do not match cache");

    for (int y = 0; y < height_; y++) {
        for (int x = 0; x < width_; x++) {
            for (int ch = 0; ch < n_channels_; ch++) {
                float value = aligned.at(x, y, ch);
                mapped_[offset(x, y, ch, frame_index)] = encode(value);
                // An empty mask means the frame was cloned, not warped, so it
                // covers itself completely.
                const bool covered = coverage.empty()
                                   || coverage.covered(ch, y, x);
                const size_t b = cov_bit(x, y, ch, frame_index);
                const std::uint8_t bit = static_cast<std::uint8_t>(1u << (b & 7));
                if (covered) coverage_bits_[b >> 3] |= bit;
                else         coverage_bits_[b >> 3] &= static_cast<std::uint8_t>(~bit);
            }
        }
    }

    // Schedule writeback of THIS frame's bytes, and nothing else.
    //
    // Dirty pages have to be bounded: on 2026-09-05 a 33-frame 24 MP OSC
    // cache held 5.2 GB of dirty page cache beside a 14.8 GB voxel cube and
    // PixInsight was OOM-killed mid-cache. Under the old pixel-major layout
    // bounding them per frame was ruinous -- one frame's writes were strided
    // across the whole mapping, so an msync of it pushed the entire cache
    // file back to disk for the 66 MB that frame actually held. Frame-major
    // makes the frame's dirty pages contiguous, so the bound is per-frame
    // again and costs one frame's worth of I/O.
    const std::size_t frame_elems =
        static_cast<std::size_t>(width_) * height_ * n_channels_;
    const std::size_t values_begin =
        frame_elems * static_cast<std::size_t>(frame_index) * sizeof(uint16_t);
    sync_range(values_begin, values_begin + frame_elems * sizeof(uint16_t));

    // The coverage bitplane is a second contiguous range, one bit per entry,
    // indexed identically to the values.
    const std::size_t plane_base =
        frame_elems * static_cast<std::size_t>(max_frames_) * sizeof(uint16_t);
    sync_range(plane_base + (frame_elems * static_cast<std::size_t>(frame_index)) / 8,
               plane_base + (frame_elems * static_cast<std::size_t>(frame_index + 1) + 7) / 8);
    ++sync_count_;

    frame_map_.push_back(global_index);
    // Published after the pixels are in place: Phase B reads
    // n_frames_written_ to decide how many slots are real.
    n_frames_written_.store(frame_index + 1, std::memory_order_release);
    return frame_index;
}

void FrameCache::flush() {
    if (!mapped_) return;
    msync(mapped_, mapped_size_, MS_ASYNC);
    ++sync_count_;
}

bool FrameCache::read_frame_range(int f, int start_pixel, int count, int ch,
                                  float* out_values,
                                  std::uint8_t* out_valid) const {
    if (!mapped_ || out_values == nullptr) return false;
    if (ch < 0 || ch >= n_channels_) return false;
    if (count <= 0 || start_pixel < 0) return false;

    const int n_written = n_frames_written_.load(std::memory_order_acquire);
    if (f < 0 || f >= n_written) return false;

    const int64_t n_pixels = static_cast<int64_t>(width_) * height_;
    if (static_cast<int64_t>(start_pixel) + count > n_pixels) return false;

    // One frame's samples are consecutive, so a whole frame-row of one
    // channel is a single span at a stride of n_channels_ -- and stride 1 for
    // the mono case. Two integer divisions per pixel is what this replaces.
    const size_t base = offset(start_pixel % width_, start_pixel / width_, ch, f);
    const size_t stride = static_cast<size_t>(n_channels_);
    for (int i = 0; i < count; ++i) {
        const size_t e = base + static_cast<size_t>(i) * stride;
        out_values[i] = decode(mapped_[e]);
        if (out_valid)
            out_valid[i] = static_cast<std::uint8_t>(
                (coverage_bits_[e >> 3] >> (e & 7)) & 1u);
    }
    return true;
}

} // namespace nukex
