#pragma once

#include "nukex/io/image.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/gpu/gpu_config.hpp"
#include <memory>
#include <string>
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
class ColorComposer;
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

        /// Path to the shipped QE database JSON. The default resolves at
        /// startup relative to the working directory; in a PI module install
        /// that becomes <plugin>/share/qe_database.json, which is correct.
        ///
        /// Layered responsibility for the failure-at-default case:
        ///   - The constructor loads eagerly and captures any failure in
        ///     qe_load_error_. On the first execute() call with non-empty
        ///     light_paths, that error is re-emitted and ok=false is
        ///     returned. This is the INTENDED behaviour until production
        ///     assets land:
        ///   - Task 16 ships the actual share/qe_database.json alongside
        ///     the PI module, at which point the default path resolves
        ///     correctly for end users.
        ///   - Task 12 surfaces the qe_load_error_ string in the module
        ///     status bar (NukeXInstance::ExecuteGlobal currently sees
        ///     result.ok==false but the user only sees "** No frames were
        ///     processed."). Until Task 12 lands the error is in the Process
        ///     Console log, not the UI.
        ///   - Tests MUST override this field to point at a fixture — see
        ///     fs::path(NUKEX_TEST_FIXTURES_DIR) / "qe" / "minimal_db.json"
        ///     in test/unit/stacker/test_stacking_engine.cpp for the canonical
        ///     pattern — or they will fail with a QE-load error on the first
        ///     frame.
        std::string           qe_database_path = "share/qe_database.json";

        std::string           qe_override_path; // optional; empty = none
        GPUExecutorConfig     gpu_config;
    };

    explicit StackingEngine(const Config& config);
    ~StackingEngine();  // out-of-line so unique_ptr<incomplete> can compile

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
        int                    n_frames_processed        = 0;
        int                    n_frames_failed_alignment = 0;

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
    std::unique_ptr<ColorComposer>        composer_;
    std::string                           qe_load_error_;  // surfaced in execute() if non-empty
};

} // namespace nukex
