// NukeX v5 — Distribution-Fitted Stacking for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.
#pragma once

#include <cstddef>
#include <vector>

namespace nukex {

/// One frame's sky, as the measuring pass reports it.
struct FrameSky {
    double location = 0.0;   ///< robust background level (median)
    double scale    = 0.0;   ///< robust spread (MAD x 1.4826)
    bool   usable   = true;
};

/// How to bring one frame onto the common scale: v' = scale*v + offset.
struct NormalizationCoefficients {
    double scale  = 1.0;
    double offset = 0.0;
};

/// Solve additive + multiplicative normalisation for a batch of frames.
///
/// Sky level and transparency vary across a session, and NukeX currently does
/// nothing about it: EXPTIME is read, stored and summed for bookkeeping, and
/// never used to scale anything. The per-voxel fit then meets a bimodal
/// population and treats the minority as outliers.
///
/// Measured on the user's own data, both halves of that:
///   * a 156-frame M63 batch mixes 120 s and 300 s frames -- sky 2685 vs 6813
///     ADU, a ratio of 2.54 against the 2.50 exposure ratio at identical gain;
///   * within the 114-frame subset, per-frame sky offsets still run -0.8 to
///     +3.2 sigma, and upper-mixture-component membership is 60x more
///     frame-structured than chance.
///
/// The reference is the MEDIAN location and scale over usable frames, not a
/// chosen frame: one fogged or blown exposure must not rescale the batch.
///
/// A frame with no measurable scale, or one marked unusable, gets the identity
/// -- normalising on a scale of zero would be division by noise.
std::vector<NormalizationCoefficients>
solve_frame_normalization(const std::vector<FrameSky>& frames);

/// Measure one channel's sky: median location, and a GRADIENT-BLIND scale.
///
/// Median rather than mean because a light frame is sky plus a bright
/// minority. A mean-and-sigma estimator reads the stars as the level, so the
/// batch would end up normalised on how many stars each frame happens to
/// show -- on the seeing -- instead of on its sky.
///
/// The scale is the MAD of horizontal nearest-neighbour DIFFERENCES over
/// background pixels, / sqrt(2), not the MAD about the median. That
/// distinction decides whether this works at all. `scale` is what the solver
/// divides by, so it has to be noise; MAD about the median cannot separate
/// noise from structure, and on the user's own M27 2025 B frames the seven
/// clouded exposures reported 375-1159 where their actual pixel noise was
/// 120-169. Normalising on that crushed them by a factor of eight and made
/// the stack measurably worse: pixel noise rose 8%. The tell is the exponent
/// -- d log(MAD)/d log(sky) came out 2.31, which no photon noise can do; the
/// difference-based estimator gives 0.95.
///
/// Non-finite samples are dropped rather than propagated: a NaN reaching
/// nth_element poisons the ordering, and flats divide, so NaN and Inf are
/// both reachable from a bad calibration frame. A channel with no finite
/// samples comes back `usable == false`.
///
/// A genuinely flat channel reports scale 0, which is what
/// solve_frame_normalization reads as "hand this frame the identity". That
/// must not be fudged to an epsilon -- doing so turns "could not measure"
/// into "divide by noise".
FrameSky measure_channel_sky(const float* data, int width, int height);

/// The effective map for a slot SYNTHESIZED from already-normalised planes.
///
/// OSC's L slot is 0.299R + 0.587G + 0.114B, and Phase A builds it from
/// planes that have already been corrected -- so it must not be corrected a
/// second time. Phase B's noise model still needs to know what map was
/// effectively applied, to convert a sample back to the raw ADU its Poisson
/// term assumes.
///
/// Exact when the input planes share one correction, which is the common
/// case; a weighted mixture otherwise, because a mixture of differing affine
/// maps is not itself an affine map of the unmixed value. The residual is
/// second order and confined to the noise estimate, never to a pixel.
NormalizationCoefficients
mix_coefficients(const NormalizationCoefficients* c, const double* w, int n);

} // namespace nukex
