#pragma once

#include <vector>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <algorithm>

namespace nukex {

/// Lightweight float32 multi-channel image.
///
/// Channel-by-channel, row-major storage: data layout is [ch][y][x].
/// This matches PCL's Image layout (channel-by-channel), not OpenCV's
/// interleaved BGR layout.
///
/// This is NOT PCL's Image class. It exists so lib/io can be tested
/// independently without PixInsight. Conversion to/from PCL Image
/// happens in the module layer.
class Image {
public:
    Image() = default;

    /// Construct with dimensions and channel count. All pixels initialized to 0.
    Image(int width, int height, int n_channels);

    /// Copy and move
    Image(const Image& other);
    Image& operator=(const Image& other);
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    /// Dimensions
    int width() const { return width_; }
    int height() const { return height_; }
    int n_channels() const { return n_channels_; }
    int total_pixels() const { return width_ * height_; }

    /// Access a single pixel value at (x, y, channel).
    float& at(int x, int y, int ch) {
        return data_[ch * width_ * height_ + y * width_ + x];
    }
    float at(int x, int y, int ch) const {
        return data_[ch * width_ * height_ + y * width_ + x];
    }

    /// Pointer to the start of a channel's pixel data.
    float* channel_data(int ch) {
        return data_.data() + ch * width_ * height_;
    }
    const float* channel_data(int ch) const {
        return data_.data() + ch * width_ * height_;
    }

    /// Pointer to all pixel data (contiguous: [ch0 pixels][ch1 pixels]...).
    float* data() { return data_.data(); }
    const float* data() const { return data_.data(); }

    /// Total number of float values in storage (width * height * n_channels).
    size_t data_size() const { return data_.size(); }

    /// Create a deep copy.
    Image clone() const;

    /// Apply a function to every pixel value in-place.
    template<typename Fn>
    void apply(Fn&& fn) {
        for (size_t i = 0; i < data_.size(); i++) {
            data_[i] = fn(data_[i]);
        }
    }

    /// Fill all pixels with a value.
    void fill(float value);

    /// Check if image has been allocated.
    bool empty() const { return data_.empty(); }

private:
    int width_      = 0;
    int height_     = 0;
    int n_channels_ = 0;
    std::vector<float> data_;
};

} // namespace nukex
