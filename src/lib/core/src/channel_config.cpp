#include "nukex/core/channel_config.hpp"

namespace nukex {

ChannelConfig ChannelConfig::from_mode(StackingMode mode) {
    ChannelConfig cfg;
    cfg.mode = mode;
    switch (mode) {
        case StackingMode::MONO_L:
            cfg.n_channels = 1;
            cfg.channel_names[0] = "L";
            cfg.bayer = BayerPattern::NONE;
            cfg.output_rgb_mapping[0] = 0;
            cfg.output_rgb_mapping[1] = 0;
            cfg.output_rgb_mapping[2] = 0;
            break;
        case StackingMode::MONO_LRGB:
            cfg.n_channels = 4;
            cfg.channel_names[0] = "L";
            cfg.channel_names[1] = "R";
            cfg.channel_names[2] = "G";
            cfg.channel_names[3] = "B";
            cfg.bayer = BayerPattern::NONE;
            cfg.output_rgb_mapping[0] = 1;
            cfg.output_rgb_mapping[1] = 2;
            cfg.output_rgb_mapping[2] = 3;
            break;
        case StackingMode::OSC_RGB:
            cfg.n_channels = 3;
            cfg.channel_names[0] = "R";
            cfg.channel_names[1] = "G";
            cfg.channel_names[2] = "B";
            cfg.bayer = BayerPattern::RGGB;
            cfg.output_rgb_mapping[0] = 0;
            cfg.output_rgb_mapping[1] = 1;
            cfg.output_rgb_mapping[2] = 2;
            break;
        case StackingMode::OSC_HAO3:
            cfg.n_channels = 2;
            cfg.channel_names[0] = "Ha";
            cfg.channel_names[1] = "OIII";
            cfg.bayer = BayerPattern::RGGB;
            cfg.output_rgb_mapping[0] = 0;
            cfg.output_rgb_mapping[1] = 1;
            cfg.output_rgb_mapping[2] = 1;
            break;
        case StackingMode::OSC_S2O3:
            cfg.n_channels = 2;
            cfg.channel_names[0] = "SII";
            cfg.channel_names[1] = "OIII";
            cfg.bayer = BayerPattern::RGGB;
            cfg.output_rgb_mapping[0] = 0;
            cfg.output_rgb_mapping[1] = 1;
            cfg.output_rgb_mapping[2] = 1;
            break;
        case StackingMode::CUSTOM:
            cfg.n_channels = 1;
            cfg.channel_names[0] = "CH0";
            cfg.bayer = BayerPattern::NONE;
            cfg.output_rgb_mapping[0] = 0;
            cfg.output_rgb_mapping[1] = 0;
            cfg.output_rgb_mapping[2] = 0;
            break;
    }
    return cfg;
}

int ChannelConfig::channel_index_for_name(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(n_channels); i++) {
        if (channel_names[i] == name) return i;
    }
    return -1;
}

bool ChannelConfig::is_mono() const {
    return mode == StackingMode::MONO_L || mode == StackingMode::MONO_LRGB;
}

} // namespace nukex
