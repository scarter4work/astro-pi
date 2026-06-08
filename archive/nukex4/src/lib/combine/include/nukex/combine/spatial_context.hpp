#pragma once
#include "nukex/io/image.hpp"
#include "nukex/core/voxel.hpp"

namespace nukex {
class Cube;
class SpatialContext {
public:
    static constexpr int WINDOW_RADIUS = 7;
    void compute(const Image& output, Cube& cube) const;
    static float sobel_gradient(const Image& img, int x, int y);
};
} // namespace nukex
