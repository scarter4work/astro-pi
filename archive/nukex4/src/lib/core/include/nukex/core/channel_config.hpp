#pragma once

#include "nukex/core/types.hpp"
#include <cstdint>
#include <string>

namespace nukex {

enum class StackingMode : uint8_t {
    MONO_L   = 0,
    MONO_LRGB = 1,
    OSC_RGB  = 2,
    OSC_HAO3 = 3,
    OSC_S2O3 = 4,
    CUSTOM   = 5
};

inline const char* stacking_mode_name(StackingMode mode) {
    switch (mode) {
        case StackingMode::MONO_L:    return "MONO_L";
        case StackingMode::MONO_LRGB: return "MONO_LRGB";
        case StackingMode::OSC_RGB:   return "OSC_RGB";
        case StackingMode::OSC_HAO3:  return "OSC_HAO3";
        case StackingMode::OSC_S2O3:  return "OSC_S2O3";
        case StackingMode::CUSTOM:    return "CUSTOM";
    }
    return "UNKNOWN";
}

enum class BayerPattern : uint8_t {
    NONE = 0, RGGB = 1, BGGR = 2, GRBG = 3, GBRG = 4
};

struct ChannelConfig {
    StackingMode mode          = StackingMode::MONO_L;
    uint8_t      n_channels    = 1;
    std::string  channel_names[MAX_CHANNELS];
    uint8_t      output_rgb_mapping[3] = {0, 0, 0};
    BayerPattern bayer         = BayerPattern::NONE;

    static ChannelConfig from_mode(StackingMode mode);
    int channel_index_for_name(const std::string& name) const;
    bool is_mono() const;
};

} // namespace nukex
