#include "nukex/core/channel_config.hpp"

#include <cstdlib>

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

ChannelConfig ChannelConfig::from_filter(const Filter& f) {
    ChannelConfig cfg;
    switch (f.cls) {
        case FilterClass::BROADBAND_L:
            cfg.n_channels = 1;
            cfg.channel_names[0] = "L";
            cfg.bayer = BayerPattern::NONE;
            break;

        case FilterClass::BROADBAND_RGB:
            cfg.n_channels = 1;
            cfg.channel_names[0] = f.name; // "R", "G", or "B"
            cfg.bayer = BayerPattern::NONE;
            break;

        case FilterClass::BROADBAND_OSC:
            cfg.n_channels = 4;
            cfg.channel_names[0] = "R";
            cfg.channel_names[1] = "G";
            cfg.channel_names[2] = "B";
            cfg.channel_names[3] = "L"; // synthesized rec709 L per-frame
            cfg.bayer = BayerPattern::RGGB; // overridden by stacking_engine if header differs
            break;

        case FilterClass::NARROWBAND_SINGLE:
            cfg.n_channels = 1;
            cfg.channel_names[0] = f.name; // "Ha", "OIII", or "SII"
            cfg.bayer = BayerPattern::NONE;
            break;

        case FilterClass::DUAL_NB_OSC:
            cfg.n_channels = 3;
            cfg.channel_names[0] = "R_" + f.name;
            cfg.channel_names[1] = "G_" + f.name;
            cfg.channel_names[2] = "B_" + f.name;
            cfg.bayer = BayerPattern::RGGB; // overridden by stacking_engine if header differs
            break;

        case FilterClass::UNKNOWN:
            // Caller is expected to fail-loud upstream; default to MONO_L for safety
            cfg.n_channels = 1;
            cfg.channel_names[0] = "L";
            cfg.bayer = BayerPattern::NONE;
            break;
    }
    return cfg;
}

ChannelConfig ChannelConfig::merge(const ChannelConfig& a, const ChannelConfig& b) {
    ChannelConfig out;
    int idx = 0;
    auto append_unique = [&](const std::string& name) {
        if (name.empty()) return;
        for (int i = 0; i < idx; ++i) {
            if (out.channel_names[i] == name) return;
        }
        // Overflow at MAX_CHANNELS means a higher-level config bug — too many
        // distinct filter classes claiming slots. Silent truncation would
        // corrupt the voxel layout downstream (Phase A/B route by slot index).
        // Abort loudly so the caller surfaces the misconfiguration.
        if (idx >= MAX_CHANNELS) std::abort();
        out.channel_names[idx++] = name;
    };
    for (int i = 0; i < a.n_channels; ++i) append_unique(a.channel_names[i]);
    for (int i = 0; i < b.n_channels; ++i) append_unique(b.channel_names[i]);
    out.n_channels = static_cast<uint8_t>(idx);
    out.bayer = (a.bayer != BayerPattern::NONE) ? a.bayer : b.bayer;
    return out;
}

int ChannelConfig::slot_index(const std::string& name) const {
    return channel_index_for_name(name);
}

} // namespace nukex
