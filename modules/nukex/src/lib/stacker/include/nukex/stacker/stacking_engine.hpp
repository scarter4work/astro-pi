#pragma once

#include "nukex/core/coverage_trim.hpp"
#include "nukex/io/image.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/gpu/gpu_config.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>
#include <vector>

// Forward declarations — keep the new color-science types out of this
// header so v4 module-layer code (src/module/filter_classifier.hpp,
// which still has its own ::nukex::FilterClass enum until Task 14)
// does not collide with ::nukex::FilterClass from core/filter.hpp.
//
// The engine state is pImpl-style here (unique_ptr) and Cube is
// likewise forward-declared so the public header doesn't transitively
// pull in core/channel_config.hpp → core/filter.hpp.
namespace nukex {
class FilterClassifier;
class QEDatabase;
class ChannelDecomposer;
class Cube;
}

namespace nukex {

class StackingEngine {
public:
    struct Config {
        FrameAligner::Config  aligner_config;
        WeightConfig          weight_config;
        ModelSelector::Config fitting_config;
        std::string           cache_dir = "/tmp";

        /// Which Phase B estimator produces each pixel. The model race is
        /// NukeX's original thesis: fit Student-t, GMM, Contamination and KDE
        /// per voxel, pick by AICc. Huber is the alternative the research
        /// branch measured against it (see HuberEstimator).
        /// Measured on the four E2E corpora (v5.0.5.3 build, same frames):
        /// Huber's stack is less noisy on every one -- ratio to the model
        /// 1.24x->1.14x mono, 1.49x->1.16x dual-NB, 2.39x->2.16x LRGB-mono,
        /// 2.69x->2.53x 24 MP OSC -- at 13-17x less Phase B time and half the
        /// total, with saturation within 1% and alignment identical. Huber is
        /// therefore the default; the race remains selectable.
        enum class Estimator { MODEL_RACE, HUBER };
        Estimator             estimator = Estimator::HUBER;

        /// Keep the voxel record in a mapped, unlinked file under cache_dir
        /// when it would crowd memory. At 604 B/voxel a 24 MP 4-channel
        /// stack is 14.8 GB; as heap that is what pushed a 30 GB machine to
        /// 6 GB available and into swap. File-backed, the same run kept
        /// 18 GB available and produced bit-identical output -- but took 30%
        /// longer (827 s against 640 s), so the file is used only when the
        /// record exceeds `file_backed_cube_fraction` of the memory available
        /// at allocation. A record that fits stays in memory at full speed.
        /// Falls back to memory, with a message, if the file cannot be made.
        bool                  file_backed_cube = true;
        double                file_backed_cube_fraction = 0.5;

        /// Selects the source of the QE database:
        ///   - EMPTY (production default): load the database compiled INTO the
        ///     module binary (nukex::embedded_qe_database_json()). There is no
        ///     file to find and no working-directory assumption, so this can
        ///     never fail with "not found" on an end user's machine.
        ///   - NON-EMPTY: load the database from this file path instead -- the
        ///     camera database downloaded by the in-module updater, or a test
        ///     fixture (see NUKEX_TEST_FIXTURES_DIR / "qe" / "minimal_db.json").
        ///
        /// Either way, the constructor loads eagerly and captures any parse
        /// failure in qe_load_error_, which the first execute() with non-empty
        /// light_paths re-emits (ok=false). qe_override_path is layered on top.
        std::string           qe_database_path; // empty => compiled-in DB

        std::string           qe_override_path; // optional; empty = none
        // Filter names the user has taught NukeX, consulted after the shipped
        // table. FITS FILTER values are whatever the capture software wrote,
        // so no shipped table can enumerate them. Empty = none.
        std::string           filter_alias_path;
        GPUExecutorConfig     gpu_config;

        /// Bring every frame onto a common sky level and spread before it is
        /// accumulated (v' = scale*v + offset, solved per cube slot).
        ///
        /// Sky level and transparency vary across a session and NukeX used to
        /// do nothing about it, so the per-voxel fit met a bimodal population
        /// and modelled a per-FRAME effect per-PIXEL. Measured across five
        /// real corpora that cost 1.00x to 5.19x the pixel-scale noise of a
        /// plain robust estimator on identical inputs.
        ///
        /// A stable session is an exact no-op, so this is on by default. It
        /// exists as a switch so the effect can be measured single-variable
        /// against a golden, which is how it was validated.
        bool                  normalize_frames = true;

        /// Remove the fixed-pattern sky tilt from the stacked output.
        ///
        /// Per-frame normalisation cannot reach this. Measured on a 65-frame
        /// NGC7635 set, the per-frame TILT is identical across frames to within
        /// 0.05-0.10 of one frame's noise while the per-frame LEVEL varies by
        /// 2.3x that: the level is sky, the tilt is fixed pattern, and stacking
        /// preserves it. A PLANE only -- a higher-order surface can follow a
        /// large nebula's outskirts and subtract real signal.
        bool                  remove_sky_gradient = true;

        /// Whether normalisation may rescale a frame, or only re-level it.
        ///
        /// OFF by default, on measurement. The additive half removes the
        /// sky-level differences the per-voxel fit was mistaking for
        /// per-pixel bimodality and cannot change a frame's signal amplitude.
        /// The multiplicative half equalises NOISE scale, and on real data it
        /// has never beaten re-levelling alone:
        ///
        ///     M27 2025 B, 24f   off 0.00026157  +scale 0.00026165  offset 0.00024208
        ///     NGC7635,    65f   off 0.00099188  +scale 0.00023512  offset 0.00023288
        ///
        /// The reason is physical. Matching noise scale is a TRANSPARENCY
        /// correction, and it is only right when a frame's noise rose because
        /// its signal fell with it. On these sessions the sky rose from added
        /// skyglow instead -- d log(scale)/d log(sky) measures 0.53, and
        /// photon noise is 0.5 -- so the signal did not change and scaling it
        /// down is simply wrong. On the B frames it widened the star-flux
        /// spread from 0.090-1.119x of the median to 0.056-1.399x.
        ///
        /// Kept available rather than deleted: a session whose scale really
        /// does track transparency (the exponent near 1.0 rather than 0.5)
        /// is the case this was designed for.
        bool                  normalize_scale  = false;
    };

    explicit StackingEngine(const Config& config);
    ~StackingEngine();  // out-of-line so unique_ptr<incomplete> can compile

    /// Phase B Q-solve output: per-pixel emission-line + broadband slot images.
    ///
    /// Built after the pixel-selector populates `stacked` with raw selected
    /// per-slot values. For dual-NB groups (HaO3 / S2O3) the raw R/G/B
    /// columns are decomposed into emission-line components via the Q matrix
    /// from ChannelDecomposer; multi-source line slots (e.g. OIII contributed
    /// by both HaO3 and S2O3 batches) are merged by sample-count weighted
    /// mean. Broadband and single-line slots pass through 1:1.
    ///
    /// `negative_clamped_count` is a diagnostic: counts the per-pixel
    /// emission values that the linear solve produced as < 0 (which is
    /// physically meaningless and gets clamped to 0). A high count usually
    /// means a degenerate Q matrix or unmodelled spectral leakage; surface
    /// it in the user-facing summary or Process Console log.
    struct DerivedStack {
        int width  = 0;
        int height = 0;
        std::unordered_map<std::string, std::vector<float>> slots;
        std::int64_t negative_clamped_count = 0;
    };

    /// Execution result.
    ///
    /// Phase 9 (color-science overhaul, Task 9): adds explicit
    /// loud-fail signalling via ok / error so the engine can refuse a
    /// batch (unknown FILTER on Bayer, missing QE DB, etc.) rather than
    /// emitting silent garbage. Pre-existing fields preserved.
    ///
    /// `cube` is held by unique_ptr (pImpl) for the same forward-decl
    /// reason — see the namespace comment above. May be nullptr on the
    /// loud-fail / empty-input paths; non-null after a successful
    /// Phase A. Callers should null-check before reading.
    struct ExecuteResult {
        bool        ok    = true;        // false on loud-fail; check before reading other fields
        std::string error;               // human-readable explanation when !ok

        Image                  stacked;
        Image                  noise_map;        // PREDICTED uncertainty (CCD model / Welford)
        Image                  measured_noise;   // REALISED pixel-to-pixel scatter
        Image                  quality_map;
        std::unique_ptr<Cube>  cube;     // populated by Phase A; consumed by Phase B (Task 10)
        DerivedStack           derived;  // Phase B Q-solve output (Task 10B)
        int                    n_frames_processed        = 0;
        int                    n_frames_failed_alignment = 0;  // real alignment misses only
        int                    n_frames_rejected_filter  = 0;  // unknown FILTER on Bayer
        // The FILTER value that stopped the batch, when that is why it
        // stopped; empty otherwise. The interface offers to learn it, and
        // needs the name to do so -- matching on the message text would break
        // the first time the wording changed.
        std::string            unknown_filter;
        bool qe_generic_camera_fallback = false; // spec 6.3: INSTRUME not in QE DB, generic Sony OSC QE used

        /// What the chroma background match subtracted, by slot name.
        ///
        /// Provenance: this is the sky level this session's filter and light
        /// pollution put on each colour channel, and a user who wants the raw
        /// colour back can add it again. Empty when nothing was matched.
        std::vector<std::pair<std::string, float>> background_match;

        /// The rectangle the intersection trim kept, in the PRE-trim
        /// coordinates the cube still uses.
        ///
        /// `stacked`, `noise_map`, `quality_map` and `derived` are already cut
        /// to it. `cube` is NOT: cropping it would mean copying a record
        /// measured in gigabytes to save a one-pixel border. Anything that
        /// reads the cube alongside a stacked pixel must add (x0, y0).
        TrimBounds trim;

        ExecuteResult();
        ~ExecuteResult();
        ExecuteResult(ExecuteResult&&) noexcept;
        ExecuteResult& operator=(ExecuteResult&&) noexcept;
        ExecuteResult(const ExecuteResult&)            = delete;
        ExecuteResult& operator=(const ExecuteResult&) = delete;
    };

    // Backwards-compat alias: pre-Phase-9 callers used `Result`.
    using Result = ExecuteResult;

    ExecuteResult execute(const std::vector<std::string>& light_paths,
                          const std::vector<std::string>& flat_paths,
                          ProgressObserver* progress = nullptr);

private:
    Config                                config_;
    std::unique_ptr<FilterClassifier>     filter_classifier_;
    std::unique_ptr<QEDatabase>           qe_database_;
    std::unique_ptr<ChannelDecomposer>    decomposer_;     // built lazily after QE load
    // ColorComposer is intentionally NOT owned by the engine. Composition
    // (DerivedSlots → 3-channel sRGB) is presentation-layer work and lives
    // in the module (NukeXInstance), where Task 12 wires it up. The engine
    // emits structured per-slot data via ExecuteResult.derived; consumers
    // own when and how to compose for display.
    std::string                           qe_load_error_;  // surfaced in execute() if non-empty
};

} // namespace nukex
