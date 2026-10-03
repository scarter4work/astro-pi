// NukeX v5 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
#pragma once

#include "nukex/compose/color_composer.hpp"
#include "nukex/io/image.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace nukex {

/// Compose the Phase B semantic slot planes into one 3-channel image.
///
/// This is the colour-science output: hue comes from the RATIO of the slots,
/// not from the raw per-channel accumulation the stacker also produces. The
/// difference is not cosmetic -- on a 156-frame OSC stack of M63 the raw
/// channels measured R 1.000 G 1.000 B 0.955 (saturation 0.080) while the
/// same target integrated in PixInsight measured 1.000 / 0.736 / 0.660 at
/// 0.337.
///
/// `compose_pixel` is an identity on the grey axis at every level (measured
/// at 0.5, 0.1, 0.05, 0.03, 0.027, 0.01 and 0.003 -- ratio 1.0000 for all of
/// them), so the result sits on the SAME linear scale as the slots and can be
/// handed to a stretch directly. There is no transfer function to undo.
///
/// `composer` is taken by reference because the caller configures its chroma
/// gate first, and because it accumulates the gamut-clip counter this run
/// reports.
///
/// Returns an empty Image when there is nothing to compose.
/// True when the slots carry chrominance -- anything beyond a lone L.
///
/// An L-only mono stack composes to a grey RGB triplet, which is right for
/// the composed DISPLAY but wrong as a stretch input: it would widen every
/// mono result from one channel to three, tripling the memory to say exactly
/// the same thing.
bool slots_have_colour(
    const std::unordered_map<std::string, std::vector<float>>& slots);

/// One-channel image of the SIGNED sky-subtracted emission total per pixel,
/// sum over lines of (line - composer.line_background_*()). Signed, not
/// clamped: clamping noise at zero lifts the sky's mean to +0.4 sigma, and
/// this plane exists to be smoothed and judged against the sky.
Image emission_total_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    const ColorComposer& composer);

/// Where the chroma gate's ramp sits on a (smoothed) emission-total plane.
///
/// `sky` is the plane's lower quartile -- the sky level even when an object
/// covers most of the frame, where the median is nebula. `sigma` is the
/// noise measured from lag-16 pixel differences (both directions, each
/// centred), which cancels structure: the MAD of the plane itself read
/// M16's nebula as noise 2.7x too large and put the gate inside the nebula.
/// The ramp is sky + 3 sigma to sky + 6 sigma.
struct GateStats {
    double sky = 0.0, sigma = 0.0, start = 0.0, full = 0.0;
    bool   valid = false;
};
GateStats gate_statistics(const Image& smoothed_total);

/// Separable box mean of a one-channel image over (2r+1)^2, edges clamped.
Image box_smooth(const Image& plane, int radius);

/// `gate_plane`, when given (one channel, same size), is the value the
/// chroma gate judges at each pixel instead of the pixel's own total --
/// the smoothed emission total, so colour follows extended structure and
/// not single-pixel noise.
Image compose_slots_to_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    ColorComposer& composer,
    const Image* gate_plane = nullptr);

/// One-channel image of the composer's own luminance per pixel
/// (ColorComposer::luminance_of): native L, else rec709 of RGB, else the
/// emission total. This is what an emission-line stack hands to the stretch.
Image compose_luminance_image(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots);

/// Compose with an EXTERNAL luminance. Hue and chroma come from the slots
/// exactly as in compose_slots_to_image -- linear line ratios through the
/// chroma gate -- but L* is taken from `luminance` (one channel, same size),
/// and the gamut walk happens once, at that L*.
///
/// With `luminance` == compose_luminance_image(slots) this is
/// compose_slots_to_image to the float rounding of the luminance plane
/// (differences in the 7th decimal). With a STRETCHED luminance it is the
/// cure for the dual-narrowband colour loss: the palette that had to be
/// walked to grey at L* 3 fits as composed at L* 50.
///
/// Returns an empty Image when `luminance` is not a one-channel image of the
/// given size.
Image compose_slots_with_luminance(
    int width, int height,
    const std::unordered_map<std::string, std::vector<float>>& slots,
    ColorComposer& composer,
    const Image& luminance,
    const Image* gate_plane = nullptr);

} // namespace nukex
