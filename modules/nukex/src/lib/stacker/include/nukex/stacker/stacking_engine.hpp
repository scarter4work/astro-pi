#pragma once

#include "nukex/io/image.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/gpu/gpu_config.hpp"
#include <cstdint>
#include <memory>
#include <string>
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

        /// Selects the source of the shipped QE database:
        ///   - EMPTY (production default): load the database compiled INTO the
        ///     module binary (nukex::embedded_qe_database_json()). There is no
        ///     file to find and no working-directory assumption, so this can
        ///     never fail with "not found" on an end user's machine.
        ///   - NON-EMPTY: load the database from this file path instead. Used by
        ///     tests to inject a controlled fixture (see NUKEX_TEST_FIXTURES_DIR
        ///     / "qe" / "minimal_db.json") and available as an advanced override.
        ///
        /// Either way, the constructor loads eagerly and captures any parse
        /// failure in qe_load_error_, which the first execute() with non-empty
        /// light_paths re-emits (ok=false). qe_override_path is layered on top.
        std::string           qe_database_path; // empty => compiled-in DB

        std::string           qe_override_path; // optional; empty = none
        GPUExecutorConfig     gpu_config;
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
        Image                  noise_map;
        Image                  quality_map;
        std::unique_ptr<Cube>  cube;     // populated by Phase A; consumed by Phase B (Task 10)
        DerivedStack           derived;  // Phase B Q-solve output (Task 10B)
        int                    n_frames_processed        = 0;
        int                    n_frames_failed_alignment = 0;  // real alignment misses only
        int                    n_frames_rejected_filter  = 0;  // unknown FILTER on Bayer

        /// Diagnostic: number of voxel-channels where Phase B's distribution
        /// fit did not converge (sparse coverage, <3 contributing frames —
        /// e.g. KDEFitter's hard n<3 floor) and were recombined via the
        /// median of the raw per-frame samples instead of the (otherwise
        /// zeroed/default) fitted true_signal_estimate. A high count means
        /// most of the batch is under-covered for robust mode-finding;
        /// surface it in the user-facing summary or Process Console log.
        std::int64_t           low_n_fallback_count      = 0;

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
