// gdg::Image -- the core's image buffer, in the Python core's own frame (PCL-free).
//
// data[(y*width + x)*channels + c], y = 0 is the FITS bottom row: exactly the C-order numpy
// array the Python core sees, shape (H, W) for mono or (H, W, C) for colour. ImageBridge (module
// side) flips vertically on the way in and out.
#pragma once

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace gdg {

struct Image {
    int width = 0;
    int height = 0;
    int channels = 1;
    std::vector<double> data;

    Image() = default;
    Image(int w, int h, int c = 1, double fill = 0.0) : width(w), height(h), channels(c) {
        if (w < 0 || h < 0 || c < 1) throw std::invalid_argument("gdg::Image: bad dimensions");
        data.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(c), fill);
    }

    size_t index(int x, int y, int c = 0) const {
        return (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) *
                   static_cast<size_t>(channels) +
               static_cast<size_t>(c);
    }
    double& at(int x, int y, int c = 0) { return data[index(x, y, c)]; }
    double at(int x, int y, int c = 0) const { return data[index(x, y, c)]; }

    size_t pixels() const { return static_cast<size_t>(width) * static_cast<size_t>(height); }
    bool same_shape(const Image& o) const {
        return width == o.width && height == o.height && channels == o.channels;
    }
};

}  // namespace gdg
