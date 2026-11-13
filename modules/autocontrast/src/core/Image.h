// AutoContrast core (PCL-free): the frame contract shared by every stage.
#pragma once

#include <cstddef>
#include <vector>

namespace ac {

// Interleaved float64 image, row-major, channels fastest: data[(y * width + x) * channels + c].
struct Image {
   int width = 0, height = 0, channels = 0;
   std::vector<double> data;

   Image() = default;
   Image(int w, int h, int c) : width(w), height(h), channels(c), data(static_cast<size_t>(w) * h * c, 0.0) {}

   double& at(int y, int x, int c)
   {
      return data[(static_cast<size_t>(y) * width + x) * channels + c];
   }
   double at(int y, int x, int c) const
   {
      return data[(static_cast<size_t>(y) * width + x) * channels + c];
   }
};

// Single-channel float64 plane, row-major: data[y * width + x].
struct Plane {
   int width = 0, height = 0;
   std::vector<double> data;

   Plane() = default;
   Plane(int w, int h) : width(w), height(h), data(static_cast<size_t>(w) * h, 0.0) {}

   double& at(int y, int x) { return data[static_cast<size_t>(y) * width + x]; }
   double at(int y, int x) const { return data[static_cast<size_t>(y) * width + x]; }
};

} // namespace ac
