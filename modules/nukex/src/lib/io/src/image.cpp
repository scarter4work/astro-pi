#include "nukex/io/image.hpp"

#include <algorithm>
#include <cstddef>

namespace nukex {

Image::Image(int width, int height, int n_channels)
    : width_(width)
    , height_(height)
    , n_channels_(n_channels)
    , data_(static_cast<size_t>(width) * height * n_channels, 0.0f)
{}

Image::Image(const Image& other) = default;
Image& Image::operator=(const Image& other) = default;
Image::Image(Image&& other) noexcept = default;
Image& Image::operator=(Image&& other) noexcept = default;

Image Image::clone() const {
    return *this;
}
Image Image::cropped(int x, int y, int w, int h) const {
    if (w <= 0 || h <= 0 || x < 0 || y < 0 ||
        x + w > width_ || y + h > height_ || n_channels_ <= 0) {
        return Image{};
    }
    Image out(w, h, n_channels_);
    for (int ch = 0; ch < n_channels_; ++ch) {
        const float* src = channel_data(ch) + static_cast<std::size_t>(y) * width_ + x;
        float*       dst = out.channel_data(ch);
        for (int row = 0; row < h; ++row)
            std::copy(src + static_cast<std::size_t>(row) * width_,
                      src + static_cast<std::size_t>(row) * width_ + w,
                      dst + static_cast<std::size_t>(row) * w);
    }
    return out;
}


void Image::fill(float value) {
    std::fill(data_.begin(), data_.end(), value);
}

} // namespace nukex
