#pragma once

#include "nukex/core/channel_config.hpp"

#include <string>

namespace nukex {

/// Which stacked planes form "the luminance" the spatial kernel measures noise
/// on, and which predicted-noise planes are therefore comparable with it.
///
/// Resolved by slot NAME, not position. LRGB-mono slots are in frame-arrival
/// order, so plane 0 is whichever filter's file sorted first; a positional
/// rec709 of planes 0..2 on such a stack was 0.2126*L + 0.7152*R + 0.0722*G
/// labelled "luminance". With an L slot present -- real on LRGB-mono,
/// synthesized on OSC -- the honest luminance is that plane itself, and the
/// prediction to compare it with is the same plane of the predicted map.
struct LuminanceSpec {
    enum Mode { AUTO_POSITIONAL = -1, SINGLE = 0, REC709 = 1 };
    int mode = AUTO_POSITIONAL;
    int c0 = 0, c1 = 1, c2 = 2;

    /// The kernels' historical behaviour: rec709 of planes 0..2 when there
    /// are three or more, else plane 0. Used when no config is known.
    static LuminanceSpec positional(int n_channels) {
        LuminanceSpec s;
        if (n_channels >= 3) { s.mode = REC709; s.c0 = 0; s.c1 = 1; s.c2 = 2; }
        else                 { s.mode = SINGLE; s.c0 = 0; }
        return s;
    }

    /// By name: an L slot wins; else R, G and B by name; else positional.
    static LuminanceSpec for_config(const ChannelConfig& cfg, int n_channels) {
        const int l = cfg.slot_index("L");
        if (l >= 0 && l < n_channels) { LuminanceSpec s; s.mode = SINGLE; s.c0 = l; return s; }
        const int r = cfg.slot_index("R"), g = cfg.slot_index("G"), b = cfg.slot_index("B");
        if (r >= 0 && g >= 0 && b >= 0 && r < n_channels && g < n_channels && b < n_channels) {
            LuminanceSpec s; s.mode = REC709; s.c0 = r; s.c1 = g; s.c2 = b; return s;
        }
        return positional(n_channels);
    }

    LuminanceSpec resolved(int n_channels) const {
        return mode == AUTO_POSITIONAL ? positional(n_channels) : *this;
    }

    std::string describe(const ChannelConfig* cfg) const {
        auto name = [&](int i) {
            return (cfg && i < static_cast<int>(cfg->n_channels)) ? cfg->slot_name(i) : "plane " + std::to_string(i);
        };
        if (mode == SINGLE) return "the " + name(c0) + " plane";
        return "rec709 of " + name(c0) + ", " + name(c1) + ", " + name(c2);
    }
};

} // namespace nukex
