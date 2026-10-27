#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Accumulate.h"
#include "Debayer.h"
#include "FrameSource.h"
#include "Measure.h"
#include "Select.h"
#include "StackTypes.h"

namespace planetary::stack {

// The whole stack in one call (spec "The engine"): Tasks 1-5's stages composed into the passes below,
// for the PlanetaryStack process (Task 7) and its window (Task 8). PCL-free, like everything here.
//
// The passes (every one reads through the hot-pixel wrapper below, so no stage sees an unrepaired frame):
//  0. Hot pixels: kHotPixelFrames frames spread evenly over the capture (frame i * N / n) are observed
//     by HotPixelMap, one map per worker, merged; Finish decides the sensor's hot sites.
//  1. Measuring: the capture's noise model (FitNoiseModel over NoiseRuns: runs of consecutive frames,
//     Noise.h, Ruling P26), then MeasureAll over every frame with it; SelectFrames (no image when it
//     diagnoses). The model is every detail measure's noise: the frames', and the regions' (the layout
//     carries it).
//  2. Aligning: BuildReference from the clean selection (suspects never enter it, Ruling P6); the
//     region layout is placed on it (PlaceRegions with the capture-level fullFrameTarget). Then one
//     streaming pass (Ruling P1) over the selected frames AND the suspects, ascending: per frame its
//     measurement luma, GlobalAligner shift, RegionMeasurer::Measure and ResidualScale::Observe.
//     Afterwards: cut = AlignmentCut over the clean frames' shifts; clean frames that fail PassesAlignment
//     (under the cut, or torn: Shift::tear > kMaxTear) are excluded
//     (reported in StackSummary::alignment); AdmitSuspects(selection, suspects' shifts, cut) decides
//     the suspects (its explanation becomes StackSummary::selection); PlanRegions over the frames kept.
//     One diagnosis is made here (below): Unaligned. The region-stage focus check (Rulings P4, P15, P20)
//     reads the kFocusFrames sharpest kept frames once more and can only warn, never refuse.
//  3. Stacking: Accumulator over every frame the plan uses (RegionPlan::Uses), each read once more,
//     DemosaicHQ, Add; Finish, cropped to the pixels some frame reached and, by at most the guard band per side,
//     past lines too few frames reached ("The crop" below), is the image.
// So the noise model's runs are read first, once per pass (35 frames per run of 32 triples, up to
// kNoiseResponses responses: 807 triples, 885 frames, of the 240 x 180-luma Jupiter 195830, 65 triples, 74
// frames, of the 1080p Moon 193142, stack-region-probe's "noise" line; both passes 1.41 s of Jupiter
// 195830's 5.29 s, 0.89 s of the Moon 193142's 30.4 s, stack-probe's phase times on the 32-thread
// development machine), then
// the capture in full once (measuring), the selected frames once (aligning) and the stacked ones once
// more (stacking); BuildReference and the focus measure read up to kReferenceFrames frames each.
// ResidualScale observes every frame of the alignment pass over kMinAlignConfidence: a superset of
// the frames stacked, since the cut and the plan come after the pass. The extra frames -- those over the
// floor that fail alignment (torn or shaken: 6 of 1750 on the 2023-02-02 Moon, Align.h) and those no region
// keeps -- can only widen the rejection tolerance, never narrow it (rejection that bites into detail
// was Task 5's critical finding); a fourth read of the stacked frames to observe exactly them was not
// worth a whole pass.
//
// Threads: `threads` workers per pass (< 1 counts as 1; fewer when MemoryPlan says so), each with its
// own reader from the factory, taking runs of consecutive frames so every reader reads forward. The
// result does not depend on the count: every stage is deterministic for any thread count except the
// Accumulator's and ResidualScale's sums, whose order changes them at float rounding (unit test:
// 1 vs 8 threads agree within 1e-6).
//
// Progress (StackTypes.h): three phases, in this order, each reported from the calling thread with a
// fixed total and a done count that only grows and ends at the total:
//   kPhaseMeasuring  done / (hot-pixel frames + kNoisePasses x the noise model's runs + every frame)
//   kPhaseAligning   done / (3 x the reference's frames + the frames of the alignment pass + as many
//                    again for PlanRegions, its regions ranked mapped onto them + up to kFocusFrames
//                    read for the focus measure); a turned-down reference anchor (Align.h) is folded
//                    into the first part
//   kPhaseStacking   done / (frames the plan uses)
// cancelled() is polled between frames in every phase (by the calling thread; workers check a flag),
// at least every 10 ms while PlanRegions ranks the regions, and once more after each phase and before
// returning. Cancel: workers stop, every thread is joined,
// every reader closed, StackCancelled is thrown -- nothing partial is returned.
//
// Errors: a reader or factory error, a frame that cannot be read, an exception from report() or
// cancelled() -- rethrown after every thread is joined. A diagnosis is not an error: it is returned.
constexpr const char* kPhaseMeasuring = "Measuring frames";
constexpr const char* kPhaseAligning = "Aligning";
constexpr const char* kPhaseStacking = "Stacking";

// Frames HotPixelMap observes (Accumulate.h: ~50-100 spread over the capture; fewer than
// kMinHotFrames marks nothing). Every frame when the capture has fewer.
constexpr int kHotPixelFrames = 64;

// ---- Memory ---------------------------------------------------------------------------------------
//
// The spec's bound: never hold the whole video; peak resident memory <= 4 GB for 4000 frames of 1080p.
// What the engine holds is O(one frame) per worker plus the stack's sums -- never O(frames x frame
// size). The part that grows with the worker count is planned here:
//  - ResidualScale (Accumulate.h): 16 bytes per luma pixel plus its own copy of the reference (4 more),
//    per instance, and an instance cannot be shared by two threads at once. Observers are therefore
//    capped by kResidualBudget, NOT by the worker count: workers check an instance out of a pool for
//    each Observe and give it back, waiting only while every instance is busy. 1080p mono: 41 MB each
//    -> 12 observers (unit test "PlanMemory caps ResidualScale observers ..."); 1080p Bayer (960 x 540
//    luma): 10 MB -> as many as workers.
//  - Per worker: the raw frame (2 bytes per sample), its luma (4 per luma pixel; measuring, 1 more for its
//    clipped-sample counts) and alignment scratch (kWorkerScratchBytes: the correlation windows, at most
//    512 x 512 complex doubles twice);
//    the regions' and the frame quality's noise variances over their rectangles (4 per luma pixel at
//    most, and three rows of doubles: Ruling P26); the noise phase's four NoisePlanes (8 bytes per
//    sampled centre, at most kNoiseResponsesPerTriple of them) and the runs it holds for its in-order
//    merge, counted for the measuring workers when larger (noiseWorkerBytes: larger on a small frame,
//    a 64 x 64 luma's 223 KB; 4K mono's 84 MB is under the 166 MB a measuring worker holds);
//    in the stacking pass the demosaic's two float planes (8 bytes per pixel), the demosaiced frame
//    with room for its prefilter (8 bytes per output sample) and the thread's clip flags (up to 5 bytes per
//    pixel). Workers per pass are capped so fixed + observers + workers stay within kMemoryBudget (a 4K RGB
//    16-bit capture: 357 MB per stacking worker beside 846 MB fixed, so 8 of them -- 813 MB at fix round 2 of
//    fix/planetary-edge, before the in-frame weight of Task A2; 1080p never hits the cap).
//  - Fixed: the Accumulator's sums (20 bytes per output sample and 13 per pixel: kept and rejected, saturated apart,
//    and the in-frame weight, Accumulate.h "Clipping", "The frame's border" and "Memory"), the reference and
//    residual scale at luma size, the regions' blend mask. Not planned, being
//    transient and outside the worker passes: PlaceRegions' scratch for the mask (RegionBlendMask: the 3 x 3 means
//    and the sky's samples, 4 + 4 bytes per luma px, and the distance transform's 8 per px of the grid's extent:
//    ~16 bytes per luma px, ~130 MB on 4K mono), freed before the alignment pass.
//  - The focus measure: its kFocusFrames luma frames (focusBytes; 4K mono 531 MB), held after the
//    residual scale is computed and its observers released, so never beside them.
//  - What grows with frames x regions (final review, engine M3), planned once the layout is placed
//    (PlanMemory's `regions`; frames: the capture's, an upper bound on the alignment pass's):
//    measureStoreBytes, the alignment pass's region measurements -- sizeof(RegionMeasure) per frame and
//    region plus per frame its FrameRegions (twice: the pass's array and the frames kept), Shift and two
//    flags -- held from the pass, beside its workers and observers, to PlanRegions; planBytes, the plan --
//    two floats and a flag per frame and region (local shift, flags) plus per frame its index, Shift, gain
//    and stacked flag, and per region its detail and window -- held from PlanRegions to the end of the
//    stack, once (the Accumulator takes it by move); planScratchBytes, PlanRegions' heap on each of its workers
//    (PlanRegionsWorkerBytes: 664 bytes per frame, 512 of them RankByDetail's likelihood table, kPriorGrid
//    doubles per frame; test_plan_memory measures it, with the frames' order PlanRegions holds once, 8 bytes per
//    frame: PlanRegionsOrderBytes), PlanRegions' workers (planWorkers) capped to fit it beside
//    the measurements, the observers and the plan. A 4K mono surface places up to 59 x 33 = 1947 regions
//    (kMaxRegionSize, half overlap): at 10 000 frames 545 MB of measurements and 175 MB of plan (unit test
//    "PlanMemory counts what grows with frames x regions"), so the alignment pass's workers are capped to
//    fit beside them. The per-frame statistics (sizeof(FrameStats) per frame) are counted with them.
// After Finish the shared HotPixelMap holds only its sites (Accumulate.h), not 2 bytes per sample.
constexpr size_t kMemoryBudget = 4000000000ull;
constexpr size_t kResidualBudget = 512000000ull;
constexpr size_t kWorkerScratchBytes = 8u << 20;

struct MemoryPlan {
    int measureWorkers = 1, alignWorkers = 1, stackWorkers = 1;
    int planWorkers = 1;               // PlanRegions' workers: alignWorkers, or fewer to fit planScratchBytes each
    int observers = 1;                 // ResidualScale instances
    size_t observerBytes = 0;          // one instance
    size_t measureWorkerBytes = 0, alignWorkerBytes = 0, stackWorkerBytes = 0;  // one worker of each pass
    size_t noiseWorkerBytes = 0;  // one noise-phase worker (FitNoiseModel), within measureWorkerBytes
    size_t fixedBytes = 0;             // Accumulator sums, reference, residual scale
    size_t focusBytes = 0;             // the focus measure's kFocusFrames luma frames
    // Frames x regions (see above; before the layout is placed, the per-frame parts only): the alignment
    // pass's measurements, the plan, and PlanRegions' scratch per worker.
    size_t measureStoreBytes = 0, planBytes = 0, planScratchBytes = 0;
    // The largest of the three passes' totals (fixed + observers + workers): what the engine itself holds
    // at its peak, beside the program's own code and the reader's buffers.
    size_t peakBytes = 0;
};
// Deterministic; threads < 1 counts as 1. Every count is at least 1, so a frame too large for the
// budget still stacks on one worker (peakBytes then exceeds kMemoryBudget and says by how much).
// regions: the layout's region count once placed (RunStack plans again with it before the alignment
// pass); 0 counts the per-frame parts of that storage only. The measuring workers do not depend on it.
MemoryPlan PlanMemory(const ser::FileInfo& info, int threads, size_t regions = 0);

// ---- Hot-pixel repair as a reader -----------------------------------------------------------------

// A FrameSource that returns every frame of `inner` repaired by `map` (HotPixelMap::Repair), so the
// stages that open their own readers through a SourceFactory -- MeasureAll, BuildReference -- see the
// same repaired frames as the passes here. Info() and FrameTimestamp() are the inner reader's.
// Not thread-safe (like any FrameSource); the map is shared read-only.
class RepairingSource : public ser::FrameSource {
public:
    RepairingSource(std::unique_ptr<ser::FrameSource> inner, std::shared_ptr<const HotPixelMap> map);
    const ser::FileInfo& Info() const override { return inner_->Info(); }
    ser::Frame ReadFrame(int64_t index) const override;
    std::optional<int64_t> FrameTimestamp(int64_t index) const override { return inner_->FrameTimestamp(index); }

private:
    std::unique_ptr<ser::FrameSource> inner_;
    std::shared_ptr<const HotPixelMap> map_;
};
// The factory every stage reads through: each reader from `base`, wrapped in a RepairingSource. A
// null reader from `base` is passed on as null (the stages report it).
SourceFactory RepairingFactory(SourceFactory base, std::shared_ptr<const HotPixelMap> map);

// ---- Diagnoses made after selection ---------------------------------------------------------------
//
// Unaligned: most of the selection fails to align -- fewer frames align to the reference (clean frames
// passing alignment -- over AlignmentCut and not torn, PassesAlignment -- plus admitted suspects) than
// UnalignedBelow(N, S) = max(1, min(need, ceil(S / 2))),
// with SelectFrames' need = max(1, min(kMinStackFrames, ceil(N / 2))) and S the clean frames selected.
// The frames do not show one scene (shake, wind, a target that changes from frame to frame); no image
// is made from the few that align. Otherwise the frames that align are stacked, however few: a
// selection of 5 on a poor night (Select keeps as few as need) whose 3 intact frames align is stacked
// from those 3, not refused (final review I1; Ruling P15: a false refusal leaves the user nothing).
// Reachable only when clean selected frames score under kMinAlignConfidence (not the reference's
// scene) or are torn (Shift::tear > kMaxTear): AlignmentCut is at most the median of the clean frames
// over that floor and in one piece, so at least half of those pass it -- with every clean frame over the
// floor and in one piece, at least ceil(S / 2) align. A selection mostly torn is Unaligned when the
// intact frames left are under the threshold (test_engine "torn frames are excluded even when they are
// most of the selection": 2 intact of 5 -> Unaligned; 6 intact of 13 -> those 6 stacked).
int64_t UnalignedBelow(int64_t frames, int64_t selected);
//
// Focus from the region stage (Rulings P4, P15). The kFocusFrames sharpest frames the stack keeps
// (FrameStats::quality; read once more after the alignment pass, with their global shifts) are split
// alternately into two half-stacks; the cross-spectrum of the halves on a window is the target's power
// spectrum with no noise in it (the halves' noise is independent) -- neither pixel noise nor 8-bit
// quantisation can read as detail (both broke RegionDetail::bestQuality as a focus signal: on the
// 2026-01-30 Jupiter its noise term read ~0, and frames blurred by 6 / 12 px read 3.7 / 6.3 px against 5.5
// sharp).
// Sub-pixel shifts are applied by the Fourier shift theorem, never by interpolation (MeasureFocus).
// The detail scale of a window is D = 1 / (2 pi sqrt(<f^2>)) over its signal power (FocusDetailScale).
//
// The verdict (FocusCheck), a measure judged against a band set from in-focus captures (Rulings P15,
// P20). No region-stage verdict refuses an image: a gross defocus is refused only by Select's own
// disc rule, or the target is not found (NoTarget).
//   InFocus     up to warnAbove, the in-focus range measured (rounded up);
//   Warned      over it: the image is made, with FocusWarning in the summary warnings;
//   NotJudged   nothing to judge on (no signal, a halo the frame cannot hold, an inversion at its limit):
//               the image is made, no verdict.
//
// A disc: the measure is its limb blur over its radius, sigma / R. One flat (untapered, DC dropped)
// window holds the whole disc on sky (MeasureDiscFocus: DiscFocusWindow, grown while the inverted blur's
// halo does not fit and the frame allows); a hard-edged disc's D tracks sqrt(sqrt(pi) R sigma), not sigma
// (review C1), so sigma is recovered by inverting an exact model of a blurred uniform disc sampled by
// 1-luma-px pixels on the same DFT grid (DiscDetailScale, LimbBlurFromDetail -- reduced to the grid's
// distinct radial frequencies, so an R 228 disc on a 1024 grid inverts in well under a second; unit test:
// discs of radius 2.7 / 9.5 / 14 luma px blurred by 0.85-4 px recovered within 8%), with R from the
// disc's half-level crossing on the reference, iterated on itself (DiscRadius: Select's box radius
// grows with the blur).
// Why sigma / R and no pixel floor (review round 2, I2): the model already removes the pixel aperture,
// and everything left -- diffraction, seeing, defocus -- scales with the image scale as R does, so the
// same optics at 1x, 1.5x, 2x, 3x (a Barlow) read the same measure. Round 1's limit max(R / 8, 3 px) was
// set by its fixed 3-px floor for every capture calibrated, so it did not scale: Jupiter's in-focus blur
// read 1.23-1.33 at 1.5-3x. Unit test (flat discs integrated over each sensor pixel (6 x 6 sub-samples),
// R 7.9 luma px at 1x, scaled 1x / 1.5x / 2x / 3x, Bayer and mono): a true sigma / R of 0.16 reads
// 0.160-0.163 at every scale, 0.5 reads 0.467-0.478 (the half-level radius sits a little inward).
// The price: once sigma passes about R / 2 the disc becomes a Gaussian blob, whose radius grows with the
// blur in any measure taken from the image, so sigma / R saturates -- 0.57-0.81 measured (synthetic discs
// R 3-24 blurred 0.75-2 R; real Jupiter 195830 blurred by 16 px 0.566). And a small disc is a blob
// already in focus: diffraction and seeing against Neptune's 2-px radius read 0.378. Ruling P20: the
// region stage never refuses; noisy, faint blobs blurred past their radius can still read above 0.76 (the
// review measured 0.78-2.8 on discs of R 0.8-2 blurred by 6-8 px), which a refusal band would have turned
// into a false OutOfFocus -- such a disc is warned, or NotJudged when its inversion reaches the model's limit.
// Measured with stack-probe (first 600 frames; --blur sigma = every frame blurred by a Gaussian of sigma
// sensor px per colour site plane, its own pixel noise put back -- a synthetic defocus):
//   in focus: Jupiter 2026-01-30 195830 / 201158 / 195438 (R 7.7-7.8 luma px) 0.164 / 0.168 / 0.171,
//     195137 (R 11.8) 0.071, Neptune 204515 (R 2.0) 0.378;
//   195830 blurred 2 / 4 / 8 / 16: 0.26 / 0.38 / 0.53 / 0.57; Neptune blurred 1 / 2 / 3: 0.49 / 0.55 /
//   0.57 (by 4 it is no longer found: NoTarget).
// kDiscWarnAbove 0.4 (Neptune's 0.378 at 851ba41b, rounded up; 0.382 at head, stack-probe): a Jupiter is
// warned from a blur of ~5 sensor px. A large one later, in sensor px: the 2022-09-17 1080p Jupiters (R
// 20.2-20.7 luma px, all frames; test/run-focus-survey.sh stack) blurred 4 / 8 / 12 / 16 read 230420 0.241
// / 0.310 / 0.409 / 0.473 (warned from 12) and 225418 0.228 / 0.305 / 0.385 (at 16 Select refuses it) --
// so 225418 at 12 px, under kGrossBlurLimit, is not warned here; step 7's warn band warns it (Ruling P15b,
// Select.h kGrossBlurWarn), and RunStack reports one focus warning: this one when it fires, else the
// selection's.
//
// A frame-filling target: the measure is the detail scale D in sensor px, pooled over Hann-tapered
// windows of kFocusWindow sensor px on the placed regions (FocusWindows, JudgeFocus; the taper's own
// <f^2> is subtracted, so a Gaussian-blurred white texture reads D = sigma). It cannot be normalised to
// the image scale: no capture records its focal length, and a fractal lunar surface has no scale of its
// own, so a sharp surface sampled twice as finely reads like the same surface blurred -- Gaussian blur
// and oversampling are the same thing to this measure. So it is never refused; it is warned above the
// in-focus range, worded "may be out of focus (or ... sampled far finer than the detail it holds)".
// Measured: in focus, Moon 2023-02-02 202030 3.42, Moon 2026-01-30 193142 5.99 (first 300-400 frames;
// 5.92 on all 2120); blurred 2 / 4 / 6: 202030 6.95 / 11.69 / 15.05, 193142 9.22 / 12.70 / 15.27;
// synthetic surface (unit test scene, mono 16-bit) blurred 1 / 3 / 12 px: 4.5 / 7.3 / 12.3.
// kSurfaceWarnAbove 6.5 (5.99, rounded up to the next half pixel).
// The bands (Ruling P15), from the in-focus readings above.
// No region-stage verdict refuses (Ruling P20): refuseFrom is +inf for discs as for frame-filling targets.
constexpr double kDiscWarnAbove = 0.4;
constexpr double kDiscRefuseFrom = std::numeric_limits<double>::infinity();
constexpr double kSurfaceWarnAbove = 6.5;
constexpr int kFocusWindow = 128;  // sensor px (64 luma px for Bayer input and an undeclared mosaic)
constexpr int kMinFocusWindow = 16;
// Frames the focus measure reads: the sharpest this many the stack keeps.
constexpr int kFocusFrames = kReferenceFrames;
// A window is judged when its signal power (the halves' cross-spectrum) is at least its noise power
// (one half-stack's power less the signal).
constexpr double kMinFocusSnr = 1.0;

// Square windows on the reference grid (luma px) the focus measure looks at: one centred on each placed
// region, or on every node of the region grid when none is placed (a featureless target); side
// min(kFocusWindow / lumaScale, the luma image's sides), moved inside the image. None when the image is
// under kMinFocusWindow.
struct FocusWindow {
    int x0 = 0, y0 = 0, side = 0;
    // Hann-tapered (texture, frame-filling targets); false: flat, DC dropped -- a disc window, the whole
    // disc on dark sky inside it (DiscFocusWindow).
    bool taper = true;
    double cx = 0, cy = 0;  // a disc window: the disc's centre on the reference grid
};
std::vector<FocusWindow> FocusWindows(const RegionLayout& layout, double lumaScale);

// Power profile of one window over a set of frames, without noise bias. The frames alternate (in the
// order given) into two half-stacks A and B; each frame's window is read at the reference window plus
// its global shift -- the integer part by position (clamped inside the frame), the rest by the Fourier
// shift theorem (no interpolation, which would colour the noise and soften the detail) -- mean-free and
// Hann-tapered, and its FFT (side: the window's next fast size) added to its half.
struct FocusProfile {
    int side = 0, frames = 0;  // FFT side; frames used (fewer than 2 leaves the profile empty)
    // Per annulus r (bins whose |k| rounds to r; r = 0 .. side / 2), for plots:
    //   cross[r] = sum Re(A conj B) / (nA nB): the signal power;
    //   power[r] = sum (|A|^2 / nA^2 + |B|^2 / nB^2) / 2: signal plus one half's noise.
    std::vector<double> cross, power;
    // The same over every bin (corners included), f in cycles per luma px: c0 = sum of cross, c2 = sum
    // f^2 cross, p0 = sum of power; w0, w2: the sum and the f^2-weighted sum of the taper's own power.
    double c0 = 0, c2 = 0, p0 = 0, w0 = 0, w2 = 0;
};
// lumas: MeasurementLuma of each frame (one size); shifts: their GlobalAligner shifts. Windows are
// measured on `threads` threads (ParallelBands); the result is identical for any count.
// std::invalid_argument on a size mismatch or a window outside the frames.
// poll(): called on the calling thread between frames for a single window, and between groups of
// windows otherwise (it may throw StackCancelled; no thread is left running).
std::vector<FocusProfile> MeasureFocus(const std::vector<ser::FloatImage>& lumas, const std::vector<Shift>& shifts,
                                       const std::vector<FocusWindow>& windows, int threads,
                                       const std::function<void()>& poll = {});

struct FocusVerdict {
    double detailScale = 0;  // sensor px, pooled over the windows judged (+inf: no detail left)
    int judged = 0;          // windows judged (kMinFocusSnr)
};
// lumaScale: LumaScale(info).
FocusVerdict JudgeFocus(const std::vector<FocusProfile>& profiles, double lumaScale);
// One window's detail scale in luma px (+inf without signal) and its signal-to-noise power ratio.
double FocusDetailScale(const FocusProfile& p, double* snr = nullptr);

// The disc window: square, side a fast size -- the next one >= 4 R + 16 luma px (the disc and its
// blurred halo on sky), or the largest that fits the image -- centred on (cx, cy) and moved inside the
// width x height luma image; taper false; (cx, cy) kept in it. Side 0 when the image has no room for
// the disc and 8 px of sky around it (2 R + 16), or is under kMinFocusWindow.
FocusWindow DiscFocusWindow(double cx, double cy, double radius, int width, int height);
// The detail scale (luma px, FocusDetailScale's definition without DC) of a uniform disc of `radius`
// blurred by a Gaussian `sigma` and sampled by 1-luma-px pixels, on an n x n DFT grid:
//     power(k) = (2 J1(x) / x)^2 exp(-4 pi^2 sigma^2 f^2) sinc^2(fx) sinc^2(fy),  x = 2 pi f radius
// (the luma pixel's own aperture: a 2 x 2 mean of sensor pixels for Bayer luma, a sensor pixel for mono,
// is 1 luma px either way). Increasing in sigma.
double DiscDetailScale(double radius, double sigma, int n);
// Its inverse in sigma (bisection, to 1e-3 luma px): the limb blur of a disc of `radius` whose window
// (FFT side n) measured detail scale `detail`. 0 when the disc reads sharper than a sharp model disc
// (texture, a hard-edged moon), maxSigma when blurrier than the model at maxSigma = 4 radius + 8.
double LimbBlurFromDetail(double detail, double radius, int n, const std::function<void()>& poll = {});
// The disc's radius where its azimuthally averaged profile (0.25 px rings about (cx, cy)) first falls
// to half way between its interior level (pixels within max(1, r0 / 2) of the centre) and the sky (the
// median of `window`'s border pixels). A Gaussian blur leaves a disc edge's half-level crossing where
// it was while sigma << the radius (unit test: within 0.13 sigma, inward, for sigma <= radius / 2; a
// disc blurred past its radius is a blob and reads up to 0.6 sigma wide), where
// Select's box -- grown by the blurred halo -- does not: the 2026-01-30 Jupiter's box radius went 9.5
// -> 16.2 luma px blurred by 12 sensor px. r0: a first guess (Selection::targetRadius). 0 when no
// crossing is found inside the window.
double DiscHalfRadius(const ser::FloatImage& luma, double cx, double cy, double r0, const FocusWindow& window);
// DiscHalfRadius iterated from r0 on its own estimate, each time on that radius's DiscFocusWindow, until
// it moves under 0.05 px (at most 6 steps): the interior level is then the disc's own centre, not a ring
// of its blurred halo. r0 only seeds it -- Selection::targetRadius, of whichever definition (the blurred
// box, or the half-contrast radius); unit test: started from 0.7 R or 1.8 R + 2 sigma it lands within
// 5% of R + 0.1 px of itself for sigma <= R / 2. r0 when no crossing is found.
double DiscRadius(const ser::FloatImage& luma, double cx, double cy, double r0);

// The focus verdict (Rulings P15, P20 -- never a refusal): Warned (the
// image, with FocusWarning in the summary warnings), InFocus, or NotJudged (nothing to judge on:
// stacked, no verdict).
enum class FocusOutcome { NotJudged, InFocus, Warned };
struct FocusCheck {
    FocusOutcome outcome = FocusOutcome::NotJudged;
    bool disc = false;
    // The judged measure and its bands: disc -- limb blur over the disc's radius (dimensionless);
    // frame-filling -- the detail scale, sensor px. Warned above warnAbove; refuseFrom is +inf for both
    // (no region-stage verdict refuses, Ruling P20).
    double measure = 0, warnAbove = 0, refuseFrom = 0;
    double limbBlur = 0;     // disc: limb blur sigma, sensor px
    double radius = 0;       // disc: DiscRadius on the reference (its seed when no crossing is found), luma px
    double detailScale = 0;  // sensor px (disc: of the disc window; frame-filling: pooled)
    int windows = 0, judged = 0;
    std::string note;        // why a disc was not judged (empty otherwise)
};
// One disc window's profile (MeasureFocus of DiscFocusWindow) of a disc of `radius` luma px. NotJudged,
// with `note`, when the inverted blur's 3-sigma halo leaves the window around the disc's centre (the cut
// halo reads sharper than it is), and when the inversion reaches its 4 R + 8 limit (the detail lies
// beyond the blurred-disc model: noise, or a blob far wider than the radius it was fitted with).
// poll(): called between steps of the inversion (it may throw StackCancelled).
FocusCheck JudgeDiscFocus(const FocusProfile& profile, double radius, double lumaScale, const FocusWindow& window,
                          const std::function<void()>& poll = {});
// The disc's focus end to end: DiscFocusWindow around (cx, cy) for `radius`, MeasureFocus,
// JudgeDiscFocus -- and while the inverted blur's halo does not fit the window but a larger one fits the
// frame, again on a window of side >= 2 (R + 3 sigma) + 8 (at most twice more). NotJudged with `note`
// when the frame has no room for the disc and 8 px of sky. profile / window: what was measured last
// (may be null).
FocusCheck MeasureDiscFocus(const std::vector<ser::FloatImage>& lumas, const std::vector<Shift>& shifts, double cx, double cy,
                            double radius, double lumaScale, int threads, const std::function<void()>& poll = {},
                            FocusProfile* profile = nullptr, FocusWindow* window = nullptr);
// The texture windows' profiles (MeasureFocus of FocusWindows) of a frame-filling target.
FocusCheck JudgeSurfaceFocus(const std::vector<FocusProfile>& profiles, double lumaScale);
// The summary warning of a Warned check (one ASCII line with its numbers); empty otherwise.
std::string FocusWarning(const FocusCheck& check);

// The alignment line's region part: how many regions, how many frames each stacks of its own (of the
// alignedFrames kept), and the regions that keep no frame, which take the plain mean of the stackedFrames
// (the Accumulator's fallback where no region keeps a frame), with the reason: no detail above the
// measurement noise, or too few frames aligned there to rank (RegionDetail::tooFewFrames: every region of a
// 1-4 frame capture -- final review, engine M2). Sky-only regions
// (RegionDetail::skyOnly) are not counted as placed on the target's detail: a last clause names them.
std::string RegionPhrase(const std::vector<RegionDetail>& detail, int64_t alignedFrames, int64_t stackedFrames);

// ---- The crop (Rulings R9, R11, R19) --------------------------------------------------------------
//
// The stack is cropped in two steps, both from Accumulator::InFrameWeight -- per pixel, the share of its stacking
// weight that came from inside the frames' stacked parts: the blend weights of the frames a pixel stacks sum to 1, so
// it is 1 where every one of them reached the pixel and 0 where none did.
//  1. Unreached lines (Ruling R11): the outer rows and columns holding pixels no frame reached -- each frame's sample
//     there fell off the frame or in its guard band (Accumulate.h, "The frame's border") -- which hold only the
//     out-of-frame stand-in, a frame edge's value carried outward: invented data, so they are cut, never filled. A pixel no
//     frame reached that no outer line holds (a hole) keeps the stand-in and is reported, not cut or filled: counted
//     (StackCrop::uncovered) and named in the phrase (final review of Part A, M3).
//  2. Thin lines (Ruling R19; final review of Part A, I1): then, on each side, at most the guard band's width of further
//     lines while the outer one is thin -- the median of its in-frame share under kThinLineFraction (one half) of the
//     image's median. The guard moves every frame's stacked edge 4 px in, so on a capture held nearly still the lines
//     just inside step 1's cut are reached only by the tail of the jitter: a handful of frames, several times noisier
//     than the interior (2026-09-07 Saturn: the outermost 1-4 lines 2-4.5x the interior sky noise). A line at half the
//     weight is a stack of half the frames, about 1.4x the interior's noise. The cut is bounded by the guard's width
//     because only the guard thins those lines: an unbounded floor cut 17-26 px into a drifting capture (Task A2, the
//     64.6F), where drift, not the guard, thins the edge; a drifting edge's thinning beyond the bound stays (a stack
//     of the frames that saw it, field defect D1).
// Measured with the floor at one half (Part A final fix wave; crop left / top / right / bottom px, step 1 alone in
// brackets): 2026-09-07 Saturn 16.7F 8 / 4 / 7 / 4 (8 / 0 / 7 / 0), the outermost 4 lines' sky noise at most 1.30x the
// interior's (step 1 alone: 4.49x; 0.3.0.1: 1.1-1.2x); 17.6F 4 / 5 / 5 / 4 (3 / 3 / 4 / 2), at most 1.22x (4.17x). A floor
// of 0.3 leaves 16.7F at 2.14x; 0.4-0.7 all pass. The 64.6F Moon 5 / 6 / 5 / 3 (5 / 3 / 5 / 3), the 70.0F 7 / 4 / 9 / 2
// (7 / 0 / 9 / 2). Known limit: a few frames thrown further than the guard (unit test: 6 px) and a reference off the
// still frames' centre by a fraction of a pixel can leave a 5th thin line, which the bound keeps. Mono, RGB and an
// undeclared mosaic have no guard band (Ruling R13), so only drift can leave an edge unreached, and step 2 cuts nothing.
struct StackCrop {
    int left = 0, top = 0, right = 0, bottom = 0;  // px removed from each side of the full-resolution stack
    int guard = 0;                                 // the frames' guard band (Accumulator::Guard), for the phrase
    int64_t uncovered = 0;  // pixels left inside the crop that no frame reached (a hole no outer line holds; 0 when none)
    // Of left, top, right, bottom: the lines ThinEdgeCrop cut as thin (each <= guard); the rest no frame reached.
    int thin[4] = {0, 0, 0, 0};
};
// The crop of a width x height stack from Accumulator::InFrameWeight: the outer row or column with the largest share of
// pixels no frame reached (in-frame weight 0) is peeled, one at a time (left, top, right, bottom on a tie), while any
// outer line holds one. guard: Accumulator::Guard. std::invalid_argument on a size mismatch.
StackCrop UncoveredCrop(const std::vector<float>& inFrame, int width, int height, int guard);
// The thin-line floor (Ruling R19): an outer line is thin while the median of its in-frame weight is under this
// fraction of the median in-frame weight of the image (inside the crop). Set by measurement ("The crop" above).
constexpr double kThinLineFraction = 0.5;
// `crop` (UncoveredCrop's) extended by the thin outer lines (Ruling R19): on each side, at most crop.guard further lines
// are peeled, one at a time and the sides in turn (left, top, right, bottom), while that side's outer line is thin --
// the median of its InFrameWeight along the line, inside the crop, under `fraction` of the median over the whole crop.
// None without a guard band (guard 0) or an image median of 0; a line is never peeled below 2 px of extent.
// `uncovered` is recounted inside the result. std::invalid_argument on a size mismatch.
StackCrop ThinEdgeCrop(const std::vector<float>& inFrame, int width, int height, const StackCrop& crop,
                       double fraction = kThinLineFraction);
// The alignment line's crop clause, one ASCII phrase ("cropped 7 px left, 6 px top: no frame reached them inside its
// stacked part ..."; with thin lines, "..., except 4 px top, which under half of the frames stacked there reached";
// without a guard band, no clause about one); empty when nothing is cropped and no pixel inside is unreached.
std::string CropPhrase(const StackCrop& crop);

// ---- The result -----------------------------------------------------------------------------------

struct StackSummary {
    int64_t measured = 0;    // frames in the capture (all measured)
    int64_t used = 0;        // frames stacked (RegionPlan::stackedCount); 0 when diagnosed
    // One ASCII line each, for the result line and FITS HISTORY:
    //  selection: AdmitSuspects' explanation (SelectFrames' when there are no suspects; the diagnosis
    //             message when Select diagnosed) -- never "(alignment decides)";
    //  alignment: the alignment outcome -- frames aligned of selected, the match and cut, the clean
    //             frames left out by AlignmentCut, the regions and how many frames each keeps, the
    //             samples rejected as outliers and the hot pixels repaired. Empty whenever there is no
    //             image: Select's diagnoses and Unaligned.
    std::string selection, alignment;
    double seconds = 0;      // wall time of the whole call
    // Wall time per step, in order: "Hot pixels", "Measuring noise", "Measuring frames", "Selecting", "Building
    // reference", "Aligning frames", "Planning regions", "Judging focus", "Stacking" (steps not reached
    // are absent).
    std::vector<std::pair<std::string, double>> phaseSeconds;
    // Frame data read over the whole call (frame payload bytes, every pass), and that over `seconds`:
    // the read rate the run sustained, so a slow run can be put down to the disk or network (~110 MB/s
    // on 1 GbE) or not.
    int64_t bytesRead = 0;
    double readMBps = 0;
    // The reader's warnings (FileInfo::warnings) and the engine's: kUndeclaredMosaicWarning when an
    // undeclared mosaic was stacked (Ruling P9; not when the capture was diagnosed -- nothing was stacked).
    std::vector<std::string> warnings;
    size_t hotPixels = 0;    // hot sensor sites repaired in every frame
    int64_t rejectedCells = 0;  // Accumulator::RejectedCells
    MemoryPlan memory;       // the plan the passes ran with
    // The region-stage focus verdict and its numbers (Rulings P4, P15). Warned: the image is made and
    // `warnings` holds FocusWarning(focus); otherwise it holds the selection's focusWarning when step 7's warn
    // band fired (Ruling P15b) -- one focus warning at most.
    FocusCheck focus;
    // What the crop removed and why (StackCrop; CropPhrase ends the alignment line when it says anything).
    StackCrop crop;
};

// Which frames went where (for Task 7's HISTORY and the tests).
struct StackFrames {
    std::vector<int64_t> stacked;             // frames the stack uses, ascending
    std::vector<int64_t> admittedSuspects;    // Admission::admitted
    std::vector<int64_t> rejectedSuspects;    // Admission::rejected
    std::vector<int64_t> alignmentExcluded;   // clean selected frames failing PassesAlignment (under the cut, or torn)
    // Per stacked frame (same order): how many regions stack it (0: only the fallback stack, where no
    // region covers). regions: the regions placed.
    std::vector<int> regionsKeeping;
    int regions = 0;
    std::vector<RegionDetail> regionDetail;   // per region (RegionPlan::detail), for the record
    std::vector<FocusWindow> focusWindows;    // the focus measure's windows and profiles (Ruling P4)
    std::vector<FocusProfile> focus;
};

struct StackOutcome {
    // The stack: full resolution, interleaved, linear, normalised like DemosaicHQ -- RGB for Bayer and
    // RGB captures, 1 channel for mono and for an undeclared mosaic (Ruling P9, warned) -- cropped by
    // summary.crop (its pixel (x, y) is the reference grid's (x + crop.left, y + crop.top); "The crop"). Empty
    // exactly when diagnosis.kind != None.
    std::optional<ser::FloatImage> image;
    Diagnosis diagnosis;
    StackSummary summary;
    ser::FileInfo info;      // the capture as read (Bayer resolved)
    StackFrames frames;
};

// Opens `path` (ser::OpenVideo with `bayer`: AVI pattern resolution exactly as SERViewer) once per
// worker and stacks it. Throws ser::SerError when the file cannot be opened or read, StackCancelled on
// cancel.
StackOutcome RunStack(const std::string& path, ser::BayerChoice bayer, int threads, const Progress& progress);
// The same over any reader factory (one reader per call; the first one's Info() is the capture). The
// path form is this with [path, bayer] { return OpenVideo(path, bayer); }.
StackOutcome RunStack(const SourceFactory& factory, int threads, const Progress& progress);

// The summary warning of an undeclared mosaic (Ruling P9): the fact, the consequence and the remedy,
// naming no tool (a script reading summary.warnings gets it as is). The windows, the console and
// HISTORY show it merged with the reader's warning, with their own remedy (stackcards::ShownWarnings).
extern const char* const kUndeclaredMosaicWarning;

}  // namespace planetary::stack
