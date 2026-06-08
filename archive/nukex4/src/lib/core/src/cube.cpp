#include "nukex/core/cube.hpp"

namespace nukex {

Cube::Cube(int w, int h, const ChannelConfig& config)
    : width(w)
    , height(h)
    , channel_config(config)
    , n_frames_loaded(0)
    , voxels_(static_cast<size_t>(w) * static_cast<size_t>(h))
{
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            voxels_[y * width + x].n_channels = config.n_channels;
        }
    }
}

} // namespace nukex
