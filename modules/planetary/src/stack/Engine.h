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
// OutOfFocus from the region stage (Ruling P4). Select judges focus from the limb of a disc; a
// frame-filling target has no limb and a small disc's limb is a few pixels, so for those two the
// region stage decides: the capture is out of focus when the sharpest frames show no detail finer than
// kMaxDetailScale sensor px on the target. Not applied to a disc of radius >= kMinJudgedRadius luma px
// (Select's limb model judges it; no such capture was measured with this measure).
//
// The measure ("detail scale", sensor px). The kFocusFrames sharpest frames the stack keeps
// (FrameStats::quality; read once more after the alignment pass, with their global shifts) are split
// alternately into two half-stacks; on square windows of kFocusWindow sensor px centred on the placed
// regions (MeasureFocus), the cross-spectrum of the halves is the target's power spectrum with no noise
// in it (the halves' noise is independent) -- so neither the pixel noise nor 8-bit quantisation, which
// region-quality measures have to estimate and subtract, can read as detail. Pooled over every window
// whose signal power is at least its noise power (kMinFocusSnr), the detail scale is
//     D = LumaScale / (2 pi sqrt(<f^2>_signal - <f^2>_taper))
// with <f^2> the power-weighted mean squared frequency; the Hann taper's own spectrum adds its <f^2> to
// the signal's (for stationary texture, exactly), so it is subtracted, and D is the target's RMS detail
// scale, independent of the window: for a white texture blurred by a Gaussian sigma, D = sigma (unit
// test: 1.99 / 2.06 for sigma 2 and 3.81 / 3.53 for sigma 4, without / with noise as strong as the
// texture). A frame-filling target where no window is judged (nothing above the noise) is out of focus.
//
// Why not RegionDetail::bestQuality, which the ruling pointed at (measured 2026-10-22, before this
// measure): on 8-bit captures its noise term reads ~0 (keptQualitySigma 0.0000-0.0002 on the 2026-01-30
// Jupiter), so quantisation steps of a smooth image read as detail -- the Jupiter blurred by sigma 6 / 12
// sensor px (without its noise put back) read 3.7 / 6.3 px against 5.5 sharp; on 16-bit synthetic
// captures the best region is the one whose noise-inflated quality is highest (weak-structure regions,
// +-35% quality sigma), and a 32-px region window caps the measurable scale near 4.7 px.
//
// The floor, measured with stack-probe (pooled D, sensor px; --blur sigma = a synthetic defocus of the
// real capture: every frame blurred by a Gaussian of sigma sensor px per colour site plane, its own
// pixel noise put back; first 300-600 frames):
//   frame-filling, in focus: Moon 2023-02-02 202030 3.42, Moon 2026-01-30 193142 5.99;
//     blurred sigma 2 / 4 / 6: 202030 6.95 / 11.69 / 15.05, 193142 9.22 / 12.70 / 15.27;
//   small discs, in focus: Neptune 2026-01-30 204515 (R 2.7 luma px) 2.72 -- blurred by 4 px it is no
//     longer found (NoTarget); Jupiter 2026-01-30 195830 / 201158 / 195438 / 195137 (R 9.5-10 luma px:
//     a 38-px disc on Bayer luma is a small disc) 7.49 / 7.52 / 7.55 / 7.53, 195830 blurred 2 / 4 / 6 /
//     8: 8.63 / 9.57 / 10.45 / 11.39 -- refused from a blur of ~5.5 px;
//   synthetic frame-filling surface (unit test, mono 16-bit): blur 1 / 3 / 12 px: 4.5 / 7.3 / 12.3, the
//     same within 0.5 px at 1% and 3% noise.
// kMaxDetailScale = 10: 1.67 x the softer real in-focus surface and under both Moons blurred by 4 px;
// for the small discs only 1.33 x the Jupiters (one night, one telescope) -- the thinnest margin here.
constexpr double kMaxDetailScale = 10.0;
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

// ---- The result -----------------------------------------------------------------------------------

struct StackSummary {
    int64_t measured = 0;    // frames in the capture (all measured)
    int64_t used = 0;        // frames stacked (RegionPlan::stackedCount); 0 when diagnosed
    // One ASCII line each, for the result line and FITS HISTORY:
    //  selection: AdmitSuspects' explanation (SelectFrames' when there are no suspects; the diagnosis
    //             message when Select diagnosed) -- never "(alignment decides)";
    //  alignment: the alignment outcome -- frames aligned of selected, the match and cut, the clean
    //             frames left out by AlignmentCut, the regions and how many frames each keeps, the
    //             samples rejected as outliers and the hot pixels repaired. Empty when Select diagnosed.
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
    // JudgeFocus's detail scale of the capture, sensor px (Ruling P4; judged for frame-filling targets
    // and small discs, reported for every capture). 0 when nothing could be judged or before that step.
    double detailScale = 0;
    // Whether that detail scale decided focus here (a frame-filling target, or a disc whose radius --
    // targetRadius, Selection::targetRadius, luma px -- is under kMinJudgedRadius).
    bool focusByRegions = false;
    double targetRadius = 0;
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
