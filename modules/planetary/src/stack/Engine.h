#pragma once
#include <cstddef>
#include <cstdint>
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
//  1. Measuring: MeasureAll over every frame; SelectFrames (no image when it diagnoses).
//  2. Aligning: BuildReference from the clean selection (suspects never enter it, Ruling P6); the
//     region layout is placed on it (PlaceRegions with the capture-level fullFrameTarget). Then one
//     streaming pass (Ruling P1) over the selected frames AND the suspects, ascending: per frame its
//     measurement luma, GlobalAligner shift, RegionMeasurer::Measure and ResidualScale::Observe.
//     Afterwards: cut = AlignmentCut over the clean frames' shifts; clean frames under it are excluded
//     (reported in StackSummary::alignment); AdmitSuspects(selection, suspects' shifts, cut) decides
//     the suspects (its explanation becomes StackSummary::selection); PlanRegions over the frames kept.
//     Two diagnoses are made here (below): Unaligned, and the region-stage OutOfFocus (Ruling P4) from
//     the kFocusFrames sharpest kept frames, read once more.
//  3. Stacking: Accumulator over every frame the plan uses (RegionPlan::Uses), each read once more,
//     DemosaicHQ, Add; Finish is the image.
// So the capture is read in full once (measuring), then the selected frames once (aligning) and the
// stacked ones once more (stacking); BuildReference and the focus measure read up to kReferenceFrames
// frames each.
// ResidualScale observes every frame of the alignment pass over kMinAlignConfidence: a superset of
// the frames stacked, since the cut and the plan come after the pass. The extra frames -- those between
// the floor and the cut (torn or shaken: 6 of 1750 on the 2023-02-02 Moon, Align.h) and those no region
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
//   kPhaseMeasuring  done / (hot-pixel frames + every frame)
//   kPhaseAligning   done / (3 x the reference's frames + the frames of the alignment pass + up to
//                    kFocusFrames read for the focus measure); a turned-down reference anchor
//                    (Align.h) is folded into the first part
//   kPhaseStacking   done / (frames the plan uses)
// cancelled() is polled between frames in every phase (by the calling thread; workers check a flag),
// and once more after each phase and before returning. Cancel: workers stop, every thread is joined,
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
//    each Observe (26 ms per 1080p mono frame) and give it back. 1080p mono: 41 MB each -> 12
//    observers; 1080p Bayer (960 x 540 luma): 10 MB -> as many as workers. Twelve observers keep up
//    with 32 workers there: per frame the alignment pass spends ~90 ms on reading, luma, global and
//    region alignment against 26 ms observing, so 32 workers need ~9 observers busy.
//  - Per worker: the raw frame (2 bytes per sample), its luma (4 per luma pixel) and alignment
//    scratch (kWorkerScratchBytes: the correlation windows, at most 512 x 512 complex doubles twice);
//    in the stacking pass the demosaic's two float planes (8 bytes per pixel) and the demosaiced frame
//    with room for its prefilter (8 bytes per output sample).
//    Workers per pass are capped so fixed + observers + workers stay within kMemoryBudget (a 4K RGB
//    16-bit capture: ~315 MB per stacking worker, so 11 of them; 1080p never hits the cap).
//  - Fixed: the Accumulator's sums (8 bytes per output sample plus 8 per pixel, its kept and
//    rejected halves), the reference and residual scale at luma size.
//  - The focus measure: its kFocusFrames luma frames (focusBytes; 4K mono 531 MB), held after the
//    residual scale is computed and its observers released, so never beside them.
// Not in the plan (they grow with the frame count, not the frame size, and are small): the frame
// statistics (~100 bytes per frame), the per-frame region measurements (20 bytes per frame and region,
// held from the alignment pass to PlanRegions) and the plan (9 bytes per frame and region) -- 4000
// frames x 324 regions: 26 + 12 MB.
constexpr size_t kMemoryBudget = 4000000000ull;
constexpr size_t kResidualBudget = 512000000ull;
constexpr size_t kWorkerScratchBytes = 8u << 20;

struct MemoryPlan {
    int measureWorkers = 1, alignWorkers = 1, stackWorkers = 1;
    int observers = 1;                 // ResidualScale instances
    size_t observerBytes = 0;          // one instance
    size_t measureWorkerBytes = 0, alignWorkerBytes = 0, stackWorkerBytes = 0;  // one worker of each pass
    size_t fixedBytes = 0;             // Accumulator sums, reference, residual scale
    size_t focusBytes = 0;             // the focus measure's kFocusFrames luma frames
    // The largest of the three passes' totals (fixed + observers + workers): what the engine itself holds
    // at its peak, beside the program's own code and the reader's buffers.
    size_t peakBytes = 0;
};
// Deterministic; threads < 1 counts as 1. Every count is at least 1, so a frame too large for the
// budget still stacks on one worker (peakBytes then exceeds kMemoryBudget and says by how much).
MemoryPlan PlanMemory(const ser::FileInfo& info, int threads);

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
// Unaligned: fewer frames align to the reference (clean frames over AlignmentCut plus admitted
// suspects) than SelectFrames' need = max(1, min(kMinStackFrames, ceil(N / 2))) -- the frames do not
// show one scene (shake, wind, a target that changes from frame to frame). Reachable only when the
// clean frames' median confidence is under kMinAlignConfidence / kAlignRelative = 0.59 (otherwise
// at least half of them pass); no image is made from the few that align.
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
// The verdict (FocusCheck), a measure judged against two bands set from in-focus captures:
//   InFocus     up to warnAbove, the in-focus range measured (rounded up);
//   Warned      over it: the image is made, with FocusWarning in the summary warnings;
//   Refused     from refuseFrom >= 2 x every in-focus reading: OutOfFocus, no image -- gross failure only;
//   NotJudged   nothing above the noise to judge on: the image is made, no verdict.
//
// A disc: the measure is its limb blur over the usable limit. One flat (untapered, DC dropped) window
// holds the whole disc on sky (DiscFocusWindow); a hard-edged disc's D tracks sqrt(sqrt(pi) R sigma),
// not sigma (review C1: by D alone a sharp Jupiter-sized disc at 1.5x the image scale read 11.6 px and
// was refused), so sigma is recovered by inverting an exact model of a blurred uniform disc on the same
// DFT grid (DiscDetailScale, LimbBlurFromDetail; unit test: discs of radius 2.7 / 9.5 / 14 luma px
// blurred by 0.85-4 px recovered within 8%), with R from the disc's half-level crossing on the reference
// (DiscHalfRadius: Select's box radius grows with the blur). The usable limit is Select's,
// max(kBlurLimit R, kBlurFloorPixels / LumaScale): R / 8 for a large disc, 3 sensor px (diffraction at
// f/20-f/35) for a small one -- the blur relative to the target's size, or to diffraction.
// Measured with stack-probe (first 600 frames; --blur sigma = every frame blurred by a Gaussian of sigma
// sensor px per colour site plane, its own pixel noise put back -- a synthetic defocus):
//   in focus: Jupiter 2026-01-30 195830 / 201158 / 195438 (R 7.9 luma px) 0.848 / 0.855 / 0.864,
//     195137 (R 11.8) 0.561, Neptune 204515 (R 2.0) 0.496;
//   195830 blurred 1 / 2 / 3 / 4 / 6 / 8 / 12: 0.98 / 1.30 / 1.54 / 1.80 / 2.11 / 2.50 / 3.08;
//   195137 blurred 2 / 4 / 8: 1.08 / 1.79 / 2.88; Neptune blurred 1 / 2 / 3: 0.64 / 0.74 / 0.86 (by 4 it
//   is no longer found: NoTarget, Select's verdict).
// kDiscWarnAbove 0.9 (the in-focus readings' 0.864, rounded up); kDiscRefuseFrom 1.75 (2 x 0.864,
// rounded up): a Jupiter blurred by ~4 sensor px or more is refused, by 1-3 px warned.
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
constexpr double kDiscWarnAbove = 0.9, kDiscRefuseFrom = 1.75;
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
std::vector<FocusProfile> MeasureFocus(const std::vector<ser::FloatImage>& lumas, const std::vector<Shift>& shifts,
                                       const std::vector<FocusWindow>& windows, int threads);

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
// width x height luma image; taper false. Side 0 when the image is under kMinFocusWindow.
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
double LimbBlurFromDetail(double detail, double radius, int n);
// The disc's radius where its azimuthally averaged profile (0.25 px rings about (cx, cy)) first falls
// to half way between its interior level (pixels within max(1, r0 / 2) of the centre) and the sky (the
// median of `window`'s border pixels). A Gaussian blur leaves a disc edge's half-level crossing where
// it was while sigma << the radius (unit test: within 0.13 sigma, inward, for sigma <= radius / 2; a
// disc blurred past its radius is a blob and reads up to 0.6 sigma wide), where
// Select's box -- grown by the blurred halo -- does not: the 2026-01-30 Jupiter's box radius went 9.5
// -> 16.2 luma px blurred by 12 sensor px. r0: a first guess (Selection::targetRadius). 0 when no
// crossing is found inside the window.
double DiscHalfRadius(const ser::FloatImage& luma, double cx, double cy, double r0, const FocusWindow& window);

// The focus verdict (Ruling P15): Refused (OutOfFocus, no image) only on gross failure, Warned (the
// image, with FocusWarning in the summary warnings), InFocus, or NotJudged (nothing to judge on:
// stacked, no verdict).
enum class FocusOutcome { NotJudged, InFocus, Warned, Refused };
struct FocusCheck {
    FocusOutcome outcome = FocusOutcome::NotJudged;
    bool disc = false;
    // The judged measure and its bands: disc -- limb blur over the usable limit (dimensionless);
    // frame-filling -- the detail scale, sensor px. Warned above warnAbove, refused from refuseFrom.
    double measure = 0, warnAbove = 0, refuseFrom = 0;
    double limbBlur = 0;     // disc: limb blur sigma, sensor px
    double usableBlur = 0;   // disc: the usable limit max(kBlurLimit R, kBlurFloorPixels / LumaScale), sensor px
    double radius = 0;       // disc: DiscHalfRadius on the reference (Select's radius when it finds none), luma px
    double detailScale = 0;  // sensor px (disc: of the disc window; frame-filling: pooled)
    int windows = 0, judged = 0;
};
// One disc window's profile (MeasureFocus of DiscFocusWindow) of a disc of `radius` luma px.
FocusCheck JudgeDiscFocus(const FocusProfile& profile, double radius, double lumaScale);
// The texture windows' profiles (MeasureFocus of FocusWindows) of a frame-filling target.
FocusCheck JudgeSurfaceFocus(const std::vector<FocusProfile>& profiles, double lumaScale);
// The summary warning of a Warned check (one ASCII line with its numbers); empty otherwise.
std::string FocusWarning(const FocusCheck& check);

// The alignment line's region part: how many regions, how many frames each stacks of its own (of the
// alignedFrames kept), and the regions without measurable detail in any frame, which take the plain
// mean of the stackedFrames (the Accumulator's fallback where no region keeps a frame).
std::string RegionPhrase(const std::vector<RegionDetail>& detail, int64_t alignedFrames, int64_t stackedFrames);

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
    //             image: Select's diagnoses, Unaligned and the region-stage OutOfFocus.
    std::string selection, alignment;
    double seconds = 0;      // wall time of the whole call
    // Wall time per step, in order: "Hot pixels", "Measuring frames", "Selecting", "Building
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
    // `warnings` holds FocusWarning(focus).
    FocusCheck focus;
};

// Which frames went where (for Task 7's HISTORY and the tests).
struct StackFrames {
    std::vector<int64_t> stacked;             // frames the stack uses, ascending
    std::vector<int64_t> admittedSuspects;    // Admission::admitted
    std::vector<int64_t> rejectedSuspects;    // Admission::rejected
    std::vector<int64_t> alignmentExcluded;   // clean selected frames under AlignmentCut
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
    // RGB captures, 1 channel for mono and for an undeclared mosaic (Ruling P9, warned). Empty exactly
    // when diagnosis.kind != None.
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

// The summary warning of an undeclared mosaic (Ruling P9), word for word (Task 7 also writes it to
// HISTORY).
extern const char* const kUndeclaredMosaicWarning;

}  // namespace planetary::stack
