#pragma once
#include <vector>

#include "Fft.h"
#include "FrameSource.h"
#include "Measure.h"
#include "Select.h"
#include "StackTypes.h"

namespace planetary::stack {

// Global alignment (spec "The engine", step 4: follow the target as it drifts, sub-pixel).
//
// Scale. Everything here is in measurement-luma pixels (LumaForMeasure: half resolution for Bayer
// input), the same pixels as FrameStats. A consumer working on full-resolution frames multiplies a
// Shift's dx, dy by LumaScale(info) (Ruling P3) -- never by its own factor.
//
// Convention. Shift (dx, dy) says where the reference's content sits in the frame:
//     frame(x + dx, y + dy) ~ reference(x, y)
// so stacking the frame onto the reference grid samples it at (x + dx, y + dy).
struct Shift {
    double dx = 0, dy = 0;
    // Normalised gradient correlation of the frame and the reference at the found shift, clamped to
    // [0, 1]: sum grad a . grad r / sqrt(sum |grad a|^2 sum |grad r|^2) over the two low-passed,
    // mean-free, tapered windows -- the share of the frame's structure that sits where the
    // reference's does. Noise barely enters (the low pass passes white noise into the gradient
    // correlation at 1 / (4 pi sigma^4) = 1/64 of its variance), so it says whether the frame shows
    // the reference's scene in one piece, not how noisy it is. A frame torn by a shake during readout
    // (part of it displaced) loses about that part's share. 0 when either window has no signal.
    double confidence = 0;
};

// Absolute floor: below it the frame does not show the reference's scene. Measured 2026-10-21 (unit
// tests): another lunar field, a disc against a surface, a noise frame score 0.05-0.22; the same
// scene under noise, blur and drift >= 0.96. A frame torn through the middle scores ~0.5.
constexpr double kMinAlignConfidence = 0.5;
// Relative cut: a frame is excluded when it matches the reference worse than this share of the
// capture's median confidence -- when a shake, tear or foreign object cost it more than 15% of the
// match a typical frame of the capture reaches. Relative because the whole capture's level depends on the seeing and the
// scale (steady frames: 0.92-0.94 on the 1080p Moon, 0.996-1 on the 240x180 Jupiter luma), the way
// SelectFrames adapts to the quality distribution. Measured 2026-10-21 on the 2023-02-02 Moon, top
// and bottom halves aligned separately: frames whose halves differ by 3.4-8.6 px (torn) score
// 0.52-0.78 = 0.53-0.80 of the median (0.983); frames whose halves differ by 1.2-2.5 px (wobble that
// local alignment takes out) 0.895-0.95 = 0.91-0.97; steady frames on six captures stay >= 0.98 of
// their median. 0.85 sits in the gap. Synthetic (unit test): a lunar frame torn through the middle
// by 8 px scores 0.65 of an intact one, its lower 60% displaced by 8 px 0.76, a 1 px wobble 0.99.
// Limit: the match is measured after the low pass, where large-scale structure still correlates
// across a few pixels -- a half-frame tear of only 4 px keeps 0.89 and is stacked; such moderate
// tears are left to local alignment and per-region ranking (Task 5).
constexpr double kAlignRelative = 0.85;
// Frames whose Shift::confidence is below the returned cut cannot be aligned and are excluded from
// the stack (spec: "Frames that cannot be aligned are excluded, not smeared in"):
//     cut = max(kMinAlignConfidence, kAlignRelative x median confidence of the frames >= the floor)
// (lower median); kMinAlignConfidence when no frame reaches the floor. Independent of order.
double AlignmentCut(const std::vector<Shift>& shifts);
// Largest correlation window side (luma px). 512 x 512 is 0.26 M samples: one or two pairs of 2-D
// FFTs (5.3 ms each, one thread) and a few Newton steps per frame. A bigger window buys no accuracy
// (already <= 0.03 px on clean synthetic data) while the cost grows with the area.
constexpr int kMaxAlignWindow = 512;
// Gaussian low pass (sigma, luma px) on both windows, applied through the cross-spectrum. It keeps
// what global alignment should follow -- the limb and the large-scale surface -- and suppresses what
// it should not: pixel noise and the fine, seeing-distorted detail that differs from frame to frame.
// A symmetric filter on both images cannot move the correlation peak, so it adds no bias.
constexpr double kAlignLowPassSigma = 1.5;
// Frames averaged into the reference: the sharpest this many of the selection (fewer when fewer
// are selected). Averaging K frames cuts the reference's noise by sqrt(K) (4x here); they are the
// sharpest frames and are aligned before averaging, so the reference stays sharp; and K frames of
// luma are held in memory (16 x 2 MB for 1080p Bayer, 16 x 8 MB for 1080p mono).
constexpr int kReferenceFrames = 16;

// A prepared reference: its window, taper and weighted spectrum are computed once, so aligning a
// frame costs one forward and one inverse FFT per correlation and a few Newton steps. Immutable
// after construction; Align() may be called from many threads at once.
//
// Method: gradient correlation of windows, refined to the continuous maximum.
//  1. Coarse position. Disc (fullFrame false): the window is centred on the target's centroid --
//     the reference's own (MeasureFrame of the reference) and the frame's FrameStats::cx, cy, both
//     rounded -- so the drift across the frame is taken out before correlating and the window only
//     has to cover the disc (side: fast size >= 1.5 x the reference's box + 16, at least 32, at
//     most kMaxAlignWindow; a disc bigger than that is aligned on its central part). A frame
//     without a target is placed where the reference is. Full frame (fullFrame true, a lunar
//     surface): the window is the largest fast size <= min(frame side, kMaxAlignWindow) on each
//     axis, centred on the frame; drift is found by the correlation itself (up to half the window).
//  2. Each window: pixels outside the image repeat the nearest edge; the weighted mean is removed
//     and a separable Tukey taper applied (flat over the central two thirds, cosine to 0 over the
//     outer sixth on each side), so the window edges do not correlate.
//  3. Cross-spectrum C = F(frame) conj(F(reference)) |w|^2 G^2: |w|^2 (w the angular frequency)
//     makes it the correlation of the two gradient fields, G is the Gaussian low pass
//     (kAlignLowPassSigma); Nyquist bins are zeroed. Why gradients: an image's power falls ~1/f^2,
//     so a plain intensity correlation is ruled by a few of the lowest frequencies -- any bright
//     blob matches any bright patch (measured: a lunar field against a disc reference scored 0.64)
//     -- while the gradient spectrum is about flat, so every scale counts, the peak is sharp, and a
//     different scene scores <= 0.22. It also cut the sub-pixel error 3-5x on the same tests.
//     The inverse FFT of C is the correlation at integer shifts; the maximum is searched within
//     +-1/4 of the window (disc: the centroids already agree to a few pixels) or +-1/2 (full frame).
//  4. Re-centring. The tapered windows weight content shifted by d unequally, which pulls the peak
//     toward 0 by ~2% of d (measured: 31.2 px read as 30.6); so while the integer peak is more than
//     1 px off (at most 3 correlations), the frame's window moves onto it and the correlation is
//     repeated; the last residual is under a pixel and its pull negligible.
//  5. Sub-pixel: the correlation is a trigonometric polynomial with coefficients C, so its value,
//     gradient and Hessian at any (x, y) are exact sums over C; Newton's method from the parabolic
//     estimate around the integer peak converges to the continuous maximum (steps capped at 0.5 px;
//     at most 20 steps; stops below 1e-4 px). No grid, so no quantisation of the result.
//  6. Shift = (frame window centre - reference window centre) + the refined peak.
// Accuracy, unit tests on exactly shifted synthetic scenes (luma px): a 34-px textured disc, integer
// and sub-pixel shifts, worst error 0.0075 noise-free; with noise 0.02 / 0.05 of full scale (signal
// ~0.6) and the frame blurred 1.0-1.6 px against a 1.0 px reference, rms 0.012 / 0.027, worst
// 0.031 / 0.055; a lunar-like surface, shifts up to 31 px, worst 0.015 noise-free, 0.026 with noise
// 0.02 and blur 0.8-1.4. Real captures (2026-10-21, every selected frame against BuildReference):
// four Jupiter / Neptune captures (luma 240x180 to 1920x1080, windows 45-100 px) confidence
// 0.957-1.000, |centroid offset - shift| median 0.03-0.14 px; three Moons (windows 480x360 to
// 512x512, drifts up to 51 px) 0.905-0.988 in steady frames, 6 torn frames of 1750 below the cut.
// Cost: 0.1-2.3 ms per disc frame; 13.8 ms per 1080p Moon frame on one thread (one or two 512 x 512
// correlations), ~600 frames/s on 16 cores.
class GlobalAligner {
public:
    // `reference`: a 1-channel luma image (BuildReference's output). fullFrame: Selection::
    // fullFrameTarget. Throws std::invalid_argument for an empty or non-1-channel image, a sample
    // count that does not match, or fullFrame false on a reference without a target.
    GlobalAligner(const ser::FloatImage& reference, bool fullFrame);

    // `frameLuma` must have the reference's size (std::invalid_argument otherwise); `frameStats` is
    // that frame's MeasureFrame result (only targetFound, cx, cy are used, and only for a disc).
    // Deterministic: the same inputs give bit-identical results on any thread.
    Shift Align(const ser::FloatImage& frameLuma, const FrameStats& frameStats) const;

    bool FullFrame() const { return fullFrame_; }
    int WindowWidth() const { return ww_; }
    int WindowHeight() const { return wh_; }

private:
    struct Peak {
        int ix = 0, iy = 0;      // integer peak
        double x = 0, y = 0;     // refined peak
        double confidence = 0;   // 0: a window without signal
        bool recentre = false;   // the integer peak is > 1 px off: correlate again around it
    };
    void Window(const ser::FloatImage& im, int cx, int cy, std::vector<Complex>& out) const;
    Peak Correlate(const ser::FloatImage& frameLuma, int fcx, int fcy, bool mayRecentre) const;

    bool fullFrame_;
    int width_, height_;   // image size
    int ww_, wh_;          // window size
    int refCx_, refCy_;    // reference window centre (image px)
    std::vector<double> taperX_, taperY_;
    std::vector<double> lowPass2_;   // G^2 per frequency bin, 0 at Nyquist
    std::vector<Complex> refSpec_;   // conj(F(reference window)) G^2
    double refEnergy_ = 0;           // sum |F(ref)|^2 G^2 / (ww wh): low-passed window energy
    Fft2D fft_;
};

// One-call form (the plan's interface): prepares a GlobalAligner and aligns one frame -- the same
// result as GlobalAligner(reference, full).Align(frameLuma, frameStats), where `full` is decided from
// the reference itself with SelectFrames' per-frame rule FillsFrame (its box touches an edge and spans at
// least kFullFrameSpan of the width or height), or true when the reference has no target. For
// many frames prepare a GlobalAligner once, with Selection::fullFrameTarget.
Shift GlobalShift(const ser::FloatImage& reference, const ser::FloatImage& frameLuma, const FrameStats& frameStats);

// The reference every frame is aligned to, in measurement-luma scale (same size as LumaForMeasure's
// output) -- the scale FrameStats and GlobalAligner work in; full-resolution consumers scale shifts
// by LumaScale(info).
// Built from the kReferenceFrames sharpest selected frames (FrameStats::quality, ties by index):
//  1. read them (one reader from `factory`, in frame order) as LumaForMeasure luma;
//  2. pass 1: align each to the sharpest frame; average those with confidence >= AlignmentCut
//     (the sharpest always) on the sharpest frame's grid;
//  3. pass 2: align each to that average (lower noise, so a better template); average again those
//     with confidence >= AlignmentCut, with the shifts re-centred on their mean -- the
//     reference sits at the mean position of the frames it is built from, so one frame's seeing
//     tilt does not set the geometry every frame is stacked onto.
// Averaging: each output pixel is the mean of the frames' bicubic (Catmull-Rom) samples at
// (x + dx, y + dy) that fall inside the frame; a pixel no frame covers takes the sharpest frame's
// sample with the coordinates clamped to the image.
// Progress phase "Building reference": (done, total) over the frames read and the two passes
// (total = 3 x frames used); cancelled() is polled between frames and throws StackCancelled.
// Throws std::invalid_argument for a diagnosed (empty) selection or a selected frame with no entry
// in `stats` (stats is MeasureAll's output, any order); ser::SerError when the factory returns no
// reader or a reader whose frame size, frame count, colour id or depth differs from `info`; and
// whatever the reader throws.
ser::FloatImage BuildReference(const SourceFactory& factory, const ser::FileInfo& info, const Selection& selection,
                               const std::vector<FrameStats>& stats, const Progress& progress);

}  // namespace planetary::stack
