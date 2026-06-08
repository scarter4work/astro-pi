#include "nukex/io/image.hpp"

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

void Image::fill(float value) {
    std::fill(data_.begin(), data_.end(), value);
}

} // namespace nukex
