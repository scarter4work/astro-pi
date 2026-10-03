#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/io/image.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace nukex {

/// A channel correction smaller than this at the field corner is not worth a
/// resample: it is below the centroid noise floor measured on real data
/// (0.058 px between blue and green on the M3 set) by a wide margin.
///
/// Shared between FrameAligner (which decides whether to warp) and the
/// console report (which decides whether to say so) -- one definition so the
/// two can never disagree about what "applied" means.
constexpr double kNegligibleChannelShiftPx = 0.01;

/// How one channel is displaced relative to the reference channel, within a
/// single frame.
///
/// This is NOT frame-to-frame alignment. It is the disagreement between the
/// colour planes of one exposure, which has two physical causes and no
/// correction anywhere else in the pipeline:
///
///   Lateral chromatic aberration -- the optics focus different wavelengths
///   at slightly different image scales. Fixed for a given rig, radial, zero
///   at the field centre and largest at the corners. Absorbed by `s`.
///
///   Atmospheric dispersion -- the atmosphere refracts blue more than red, by
///   an amount that depends on the target's altitude. It therefore DRIFTS
///   across a session, which is why this is measured per frame rather than
///   once on the stack. Absorbed by `tx`, `ty`.
///
/// Rotation is deliberately absent from the model. Neither effect rotates, so
/// a rotation term could only fit noise.
struct ChannelTransform {
    /// Which rung of the fallback ladder produced this transform. Reported to
    /// the user, so a frame that quietly degraded is visible rather than
    /// indistinguishable from one that fitted cleanly.
    enum class Fit { Identity, TranslationOnly, Affine };

    double s  = 1.0;   ///< uniform scale about the frame centre
    double tx = 0.0;   ///< translation in x, pixels
    double ty = 0.0;   ///< translation in y, pixels

    int    n_stars  = 0;     ///< stars that survived to the fit
    double residual = 0.0;   ///< median |measured - modelled|, pixels
    Fit    fit      = Fit::Identity;

    /// Exactly identity, in the sense of "there is nothing to apply".
    bool is_identity(double tol = 1e-12) const {
        return std::abs(s - 1.0) <= tol
            && std::abs(tx)      <= tol
            && std::abs(ty)      <= tol;
    }

    /// Largest displacement this transform produces within `radius` of the
    /// centre. The scale term grows with radius; the translation does not.
    double max_displacement(double radius) const {
        return std::abs(s - 1.0) * radius + std::hypot(tx, ty);
    }

    /// Map a point from reference-channel coordinates to where THIS channel
    /// images it, in the same frame. `cx`, `cy` must be the centre the fit was
    /// made about -- ChannelTransforms carries it for exactly this reason.
    void apply(double cx, double cy, float& x, float& y) const {
        const double nx = s * (static_cast<double>(x) - cx) + tx + cx;
        const double ny = s * (static_cast<double>(y) - cy) + ty + cy;
        x = static_cast<float>(nx);
        y = static_cast<float>(ny);
    }
};

/// One ChannelTransform per channel of a frame, plus the centre they share.
///
/// Empty means "nothing to do": a mono frame, or a frame with too few stars to
/// measure anything at all. Consumers must treat empty as the no-op path
/// rather than as a failure.
struct ChannelTransforms {
    std::vector<ChannelTransform> per_channel;
    double cx = 0.0;              ///< centre every fit is about, x
    double cy = 0.0;              ///< centre every fit is about, y
    int    reference_channel = 0;

    bool empty() const { return per_channel.empty(); }

    /// True when no channel moves any point within `radius` by more than
    /// `tol` pixels -- i.e. applying this would resample for nothing.
    bool negligible(double radius, double tol) const {
        for (const auto& t : per_channel)
            if (t.max_displacement(radius) > tol) return false;
        return true;
    }
};

struct ChannelRegistrationConfig {
    /// Half-width of the box each per-channel centroid is computed in.
    ///
    /// 6 gives a 13x13 box. This is NOT a comfort margin -- it was measured.
    /// Truncating a Gaussian star biases its centroid, and the bias does not
    /// cancel between channels because the same star lands on a different
    /// sub-pixel phase in each. Recovering a known +500 ppm and 0.33 px from
    /// a synthetic field, worst error across the three fitted parameters:
    ///
    ///     box radius:        4         5         6         7
    ///     FWHM 3.8 px:  0.0181    0.0035    0.0005    0.0000  px
    ///     FWHM 4.7 px:  0.0522    0.0192    0.0054    0.0012  px
    ///
    /// At radius 4 a perfectly ordinary 4.7 px star costs 0.05 px, which is
    /// half the entire acceptance budget spent on estimator bias before any
    /// real data is involved.
    int   centroid_radius = 6;

    /// Refine the box position and re-centroid this many times. The first
    /// pass centres the box on the reference channel's position, which for a
    /// displaced channel is off by the very thing being measured; each pass
    /// moves the box onto the measured centroid and shrinks the residual
    /// pull toward the box centre.
    ///
    /// Measured, with the faint case being the one that matters -- through a
    /// dual-narrowband filter a star can be five times brighter in Ha than in
    /// OIII, and that is where a single pass falls apart:
    ///
    ///     passes:              1         2
    ///     equal brightness: 0.0061    0.0118  px
    ///     red at 1/5 flux:  0.0569    0.0186  px
    ///
    /// Two costs a little on easy stars and saves a factor of three on hard
    /// ones. Do not raise it further; a third pass measurably regressed the
    /// clean case for no gain on the faint one.
    int   centroid_iterations = 2;

    /// A star with another catalog star closer than this is not used. The
    /// neighbour's wings intrude on the box and drag the centroid, and it
    /// does so by a different amount in each channel.
    ///
    /// Must exceed centroid_radius, or a neighbour sits inside the box by
    /// construction. Measured on a field where 30% of stars had a companion
    /// 7.6 px away: 0.029 px using every star, 0.006 px using only the
    /// isolated ones.
    ///
    /// Note this is a SEPARATE test from StarDetector::Config::exclusion_radius,
    /// which defaults to 5 and therefore permits exactly the neighbours that
    /// hurt here.
    int   min_neighbour_separation = 13;

    /// A star is used in a channel only when its peak in that channel stands
    /// this many sigma above the noise on the border of its own box. On a
    /// dual-narrowband frame the same star can be strong in red and invisible
    /// in blue, and centroiding noise would poison the fit.
    float min_star_snr = 3.0f;

    int   min_stars_affine      = 8;   ///< below this, drop to translation only
    int   min_stars_translation = 3;   ///< below this, give up and use identity

    /// A fitted scale further from 1 than this is not lateral colour. Real
    /// lateral colour runs a few hundred ppm; 1% is four orders of magnitude
    /// out and can only be a bad solve.
    double max_scale_deviation = 0.01;

    /// Likewise for translation. Atmospheric dispersion at these focal
    /// lengths is a fraction of a pixel.
    double max_translation_px = 5.0;

    /// Residuals beyond this many sigma are dropped and the fit repeated once.
    double clip_sigma = 3.0;
};

/// Fit one ChannelTransform per channel of `image`, against `reference_channel`.
///
/// `stars` are positions found on the reference channel -- normally the
/// catalog FrameAligner already computed, which is why this costs no extra
/// detection pass. Each star is re-centroided in each channel independently,
/// starting from its reference position.
///
/// Returns an empty ChannelTransforms for a single-channel image, or when the
/// catalog is empty. The reference channel's own entry is always exactly
/// identity, by construction rather than by fitting.
ChannelTransforms measure_channel_transforms(
    const Image& image, const StarCatalog& stars, int reference_channel,
    const ChannelRegistrationConfig& config);

inline ChannelTransforms measure_channel_transforms(
    const Image& image, const StarCatalog& stars, int reference_channel)
{
    return measure_channel_transforms(image, stars, reference_channel,
                                      ChannelRegistrationConfig{});
}

/// One line describing what channel registration did, for the Process
/// Console. Empty when there was nothing to report.
///
/// `radius` is where the displacement is quoted -- normally the frame corner,
/// which is where a scale term is largest and where a user looking at their
/// stars will notice.
///
/// ASCII only: PCL reads const char* as ISO-8859-1, so a UTF-8 character here
/// reaches the console as mojibake.
std::string describe_channel_transforms(const ChannelTransforms& ct,
                                        double radius);

} // namespace nukex
