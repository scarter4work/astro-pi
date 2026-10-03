#pragma once

#include <vector>

namespace nukex {

/// What the reference pre-pass measures about one frame.
struct FrameQuality {
    int   star_count  = 0;      ///< stars the detector kept (capped at max_stars)
    float median_fwhm = 0.0f;   ///< pixels; <= 0 when it could not be measured
    bool  usable      = true;   ///< false for blown-out / rejected frames
};

/// Fraction of the best star count a frame must reach to be considered as the
/// alignment reference. A frame below this cannot supply enough triangles for
/// the Groth matcher no matter how sharp it is.
constexpr float kReferenceCandidateBand = 0.75f;

/// Index of the frame that should become the alignment reference.
///
/// The reference used to be whichever frame the directory listing put first.
/// On an LRGB-mono set the per-filter star yield varies enormously, so that
/// was a coin flip: on M27 2025 it landed on a blue frame with 32 stars, the
/// matcher could not find a consistent triangle against the 200-star frames
/// that followed, and 71 of 72 frames aligned with zero inliers.
///
/// The rule is minimal intervention. The frame the aligner would have adopted
/// anyway -- the first usable one -- is kept whenever it is a viable
/// candidate, meaning it reaches kReferenceCandidateBand of the best star
/// count in the batch. Only when it does not is it replaced, and then by the
/// sharpest candidate; detection caps at max_stars, so candidates usually tie
/// on count and FWHM is what separates them. Ties resolve to the lowest index,
/// because the E2E goldens require the choice to be reproducible.
///
/// It deliberately does NOT always take the best frame. Measured 2026-09-04 on
/// NGC7635, where all 65 frames detect exactly 200 stars and the first frame's
/// FWHM ranks 18th of 65: moving the reference to the sharpest frame, better
/// by 0.2 px, took that corpus from 65 of 65 frames aligned to 59 of 65. Among
/// viable candidates the choice is arbitrary, and changing it costs
/// alignments and moves every existing user's output for no gain.
///
/// Returns -1 for an empty list, and 0 when no frame is usable -- degrading to
/// the historical first-frame behaviour rather than failing the stack.
int select_reference_frame(const std::vector<FrameQuality>& frames);

} // namespace nukex
