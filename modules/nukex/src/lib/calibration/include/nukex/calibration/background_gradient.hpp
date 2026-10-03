#pragma once

#include "nukex/io/image.hpp"

namespace nukex {

/// A planar sky model fitted to an image's BACKGROUND.
///
/// `dx` and `dy` are the change across the full frame width and height, in the
/// image's own units, so a model is directly comparable to the frame's noise.
struct GradientModel {
    double level = 0.0;     ///< fitted background at frame centre
    double dx    = 0.0;     ///< change from left edge to right edge
    double dy    = 0.0;     ///< change from top edge to bottom edge
    bool   valid = false;   ///< false when the image is empty or the fit degenerate
};

/// Inclusive pixel rectangle the fit may SAMPLE from. The model itself is
/// always expressed over the full frame, so subtract() applies it everywhere.
struct FitRegion {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

/// Fixed-pattern sky gradient removal for the stacked output.
///
/// Per-frame normalisation cannot address this. Measured on the user's
/// 65-frame NGC7635 set, the per-frame TILT is identical across frames to
/// within 0.05-0.10 of one frame's noise, while the per-frame LEVEL varies by
/// 2.3x that. The level is what normalisation fixes; the tilt is a fixed
/// pattern -- optics, flats, or a static light-pollution direction -- and
/// stacking preserves it. In the finished stack it spans roughly 6 sigma from
/// one corner to the other.
///
/// Only a PLANE is fitted, deliberately. A higher-order surface can follow the
/// outskirts of a large nebula and subtract it, which is the classic way an
/// automatic background extraction quietly destroys signal. A plane cannot
/// curve into an object.
class BackgroundGradient {
public:
    /// Robustly fit a plane to `channel`'s background.
    ///
    /// The sky is the DARKEST population in an astronomical image, so the fit
    /// is seeded from the darkest samples and only then refined by residual
    /// clipping. The clipping is asymmetric -- positive residuals are cut hard
    /// and negative ones loosely -- because objects are positive excursions.
    /// The residual scatter is measured on the KEPT samples only: measured on
    /// all samples, a bright object inflates it until the clip can no longer
    /// exclude the object, and the plane settles on the object's own ramp.
    /// That failure was measured at 30% coverage, not 50%.
    ///
    /// `region`, when given, restricts SAMPLING to that rectangle: the stack's
    /// thin-coverage rim is several times noisier than its interior and sits
    /// at maximum leverage for a plane, so the caller passes the rectangle
    /// every frame contributed to.
    static GradientModel fit_planar(const Image& img, int channel,
                                    const FitRegion* region = nullptr);

    /// Subtract the model's TILT from `channel`, leaving the sky level where
    /// it was. The level feeds the auto-stretch's sky target, and moving it is
    /// a separate decision from flattening.
    static void subtract(Image& img, int channel, const GradientModel& model);

    /// Peak-to-peak amplitude of the model across the frame: the difference
    /// between the brightest and darkest corner, |dx| + |dy|.
    static double amplitude(const GradientModel& model);
};

} // namespace nukex
