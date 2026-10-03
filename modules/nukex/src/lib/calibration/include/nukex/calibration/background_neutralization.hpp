#pragma once

#include "nukex/core/channel_config.hpp"
#include "nukex/io/image.hpp"
#include <cstddef>
#include <vector>

namespace nukex {

/// What a background match took off each plane, in plane order.
///
/// Worth surfacing: it is the sky level this session's filter and light
/// pollution put on each channel, and a user who wants the raw colour back can
/// add it again.
struct BackgroundOffsets {
    std::vector<float> subtracted;      // per plane, >= 0
    bool               applied = false; // false for mono, empty, or already matched
};

/// Bring every plane's sky level down to the dimmest plane's.
///
/// The green cast in a broadband stack is an ADDITIVE sky pedestal, not a
/// scaling error. Measured on a 74-frame OSC stack: the background sits at
/// G/R = 1.54, while the signal -- once each channel's own background is
/// removed -- sits at G/R = 1.07 for stars and 1.16 for nebulosity. The
/// stretch preserves colour vectors by design, so it reproduces that 1.54
/// faithfully at every brightness, and no stretch downstream can undo it: the
/// finished JPEG of M63 measured G/R = 1.52 after GHS, curves and a crop.
/// A pedestal comes off by subtraction, in linear space, or not at all.
///
/// Matching rather than removing: zeroing the sky outright would disturb
/// everything that keys off a sky level -- the composer's chroma gate, the
/// stretch's own shadow-point solve -- for no gain. Subtracting down to the
/// dimmest plane removes the cast (measured 1.54 -> 1.00) and leaves the
/// overall level exactly where that plane already put it. Matching UP would
/// invent light that was never collected.
///
/// The reference per plane is the median (robust to a target covering any
/// fraction under half the frame), sampled on a stride because a full sort of
/// a 24 MP plane costs more than the whole operation.
///
/// Fewer than two planes is a no-op: there is nothing to match against.
BackgroundOffsets match_plane_backgrounds(const std::vector<float*>& planes,
                                          std::size_t n_pixels);

/// match_plane_backgrounds over an Image's channels. Mono is a no-op.
BackgroundOffsets neutralize_channel_backgrounds(Image& img);

/// The slot positions in `cfg` whose sky levels must agree: R, G and B,
/// wherever they sit.
///
/// The cast is a CHROMA problem. Luminance is deliberately excluded -- an L
/// filter is broad, its sky level is not comparable to a single colour
/// channel's, and pulling it down to match one would darken the image for no
/// colour gain. Emission slots are excluded too: Ha and OIII are different
/// physical lines rather than a colour balance, and the composed narrowband
/// background already measures neutral. Dual-narrowband slots (R_HaO3 and
/// friends) are raw inputs to the Q-solve, not display channels.
std::vector<int> chroma_slot_indices(const ChannelConfig& cfg);

/// Match the chroma slots of a stacked image in place, leaving every other
/// slot untouched. This is the whole feature as the stacker uses it.
BackgroundOffsets neutralize_chroma_slots(Image& img, const ChannelConfig& cfg);

} // namespace nukex
