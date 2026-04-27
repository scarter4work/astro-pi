#pragma once

#include "nukex/core/types.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/voxel.hpp"
#include <vector>
#include <cstdint>

namespace nukex {

class Cube {
public:
    int           width  = 0;
    int           height = 0;
    ChannelConfig channel_config;
    int           n_frames_loaded = 0;

    Cube(int w, int h, const ChannelConfig& config);
    Cube() = default;

    SubcubeVoxel&       at(int x, int y)       { return voxels_[y * width + x]; }
    const SubcubeVoxel& at(int x, int y) const { return voxels_[y * width + x]; }

    int total_pixels() const { return width * height; }

    bool is_valid_coord(int x, int y) const {
        return x >= 0 && x < width && y >= 0 && y < height;
    }

private:
    std::vector<SubcubeVoxel> voxels_;
};

} // namespace nukex
