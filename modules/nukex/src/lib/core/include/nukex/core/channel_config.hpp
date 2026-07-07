#pragma once

#include "nukex/core/types.hpp"
#include "nukex/core/filter.hpp"
#include <cstdint>
#include <string>

namespace nukex {

enum class BayerPattern : uint8_t {
    NONE = 0, RGGB = 1, BGGR = 2, GRBG = 3, GBRG = 4
};

struct ChannelConfig {
    uint8_t      n_channels    = 1;
    std::string  channel_names[MAX_CHANNELS];
    BayerPattern bayer         = BayerPattern::NONE;

    static ChannelConfig from_filter(const Filter& f);
    static ChannelConfig merge(const ChannelConfig& a, const ChannelConfig& b);

    int channel_index_for_name(const std::string& name) const;
    int slot_index(const std::string& name) const; // == channel_index_for_name; legible alias
    // Reverse of slot_index: returns the slot name at position i.
    // i must be in [0, n_channels).
    const std::string& slot_name(int i) const { return channel_names[i]; }
};

} // namespace nukex
