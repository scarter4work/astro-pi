#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/io/flat_calibration.hpp"
#include "nukex/io/filter_classifier.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/calibration/qe_database.hpp"
#include "nukex/calibration/channel_decomposer.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/compose/color_composer.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include "nukex/combine/pixel_selector.hpp"
#include "nukex/combine/spatial_context.hpp"
#include "nukex/combine/output_assembler.hpp"
#include "nukex/gpu/gpu_executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <numeric>
#include <string>
#include <vector>

namespace nukex {

// ── Constructor ──────────────────────────────────────────────────────
//
// QE-database load uses a deferred-fail pattern: if the JSON can't be
// found / is malformed, the error is captured into qe_load_error_ and
// re-emitted at the very top of execute(). We can't throw in the
// constructor because PI module-thread construction would terminate
// the process before the user sees a Process Console message.
//
// FilterClassifier and QEDatabase are heap-allocated (pImpl) so the
// engine header can forward-declare them — that keeps v4's old module
// FilterClass enum (in src/module/filter_classifier.hpp) from
// colliding with the new color-science enum during the migration.
//
// TASK-14-COLLAPSE: when Task 14 deletes the v4 module-level enum,
// grep this codebase for "TASK-14-COLLAPSE" to find every place where
// pImpl forward declarations and unique_ptrs in the engine header /
// ExecuteResult should revert to value members + direct includes of
// nukex/core/channel_config.hpp.
StackingEngine::StackingEngine(const Config& config)
    : config_(config),
      filter_classifier_(std::make_unique<FilterClassifier>()),
      qe_database_(std::make_unique<QEDatabase>()) {
    auto r1 = qe_database_->load_shipped(config_.qe_database_path);
    if (!r1.ok) {
        qe_load_error_ = r1.error;
        return;
    }
    if (!config_.qe_override_path.empty()) {
        auto r2 = qe_database_->load_override(config_.qe_override_path);
        if (!r2.ok) {
            qe_load_error_ = r2.error;
            return;
        }
    }
    decomposer_ = std::make_unique<ChannelDecomposer>(*qe_database_);
    composer_   = std::make_unique<ColorComposer>();
}

// Out-of-line so the unique_ptr<FilterClassifier> / <QEDatabase> /
// <ChannelDecomposer> / <ColorComposer> destructors can see the full
// types from this TU's includes.
StackingEngine::~StackingEngine() = default;

// ExecuteResult special members are out-of-line for the same
// forward-decl reason — std::unique_ptr<Cube> needs the complete Cube
// type at the destructor / move-ctor / move-assign instantiation.
StackingEngine::ExecuteResult::ExecuteResult() = default;
StackingEngine::ExecuteResult::~ExecuteResult() = default;
StackingEngine::ExecuteResult::ExecuteResult(ExecuteResult&&) noexcept = default;
StackingEngine::ExecuteResult&
    StackingEngine::ExecuteResult::operator=(ExecuteResult&&) noexcept = default;

namespace {

/// Parse a FITS BAYERPAT string into a BayerPattern enum.
BayerPattern parse_bayer_pattern(const std::string& s) {
    if (s == "RGGB") return BayerPattern::RGGB;
    if (s == "BGGR") return BayerPattern::BGGR;
    if (s == "GRBG") return BayerPattern::GRBG;
    if (s == "GBRG") return BayerPattern::GBRG;
    return BayerPattern::NONE;
}

/// Compute median of channel 0 of an image.
/// Makes a copy of the channel data since median_inplace sorts in place.
float compute_frame_median(const Image& img) {
    int n = img.width() * img.height();
    if (n == 0) return 0.0f;
    std::vector<float> vals(n);
    const float* src = img.channel_data(0);
    for (int i = 0; i < n; i++) {
        vals[i] = src[i];
    }
    return median_inplace(vals.data(), n);
}

/// Compute median FWHM from alignment star catalog.
float compute_median_fwhm(const StarCatalog& catalog) {
    if (catalog.empty()) return 0.0f;
    std::vector<float> fwhms;
    fwhms.reserve(catalog.stars.size());
    for (const auto& s : catalog.stars) {
        if (s.fwhm > 0.0f) fwhms.push_back(s.fwhm);
    }
    if (fwhms.empty()) return 0.0f;
    return median_inplace(fwhms.data(), static_cast<int>(fwhms.size()));
}

/// Compute dominant shape across channels for a voxel.
void compute_dominant_shape(SubcubeVoxel& voxel, int n_ch) {
    int counts[7] = {};
    for (int ch = 0; ch < n_ch; ch++) {
        int s = static_cast<int>(voxel.distribution[ch].shape);
        if (s >= 0 && s < 7) counts[s]++;
    }
    int best = 0;
    for (int i = 1; i < 7; i++) {
        if (counts[i] > counts[best]) best = i;
    }
    voxel.dominant_shape = static_cast<DistributionShape>(best);
}

} // anonymous namespace

StackingEngine::ExecuteResult StackingEngine::execute(
    const std::vector<std::string>& light_paths,
    const std::vector<std::string>& flat_paths,
    ProgressObserver* progress)
{
    ProgressObserver& obs = progress ? *progress : null_progress_observer();

    // Empty-input shortcut runs BEFORE the QE-error surface so callers
    // probing the engine with no work (e.g. UI initial state) don't
    // get spurious error toasts. The default Config's qe_database_path
    // points at a relative "share/" that may not exist outside the
    // PI plugin install — that's fine until the user actually feeds
    // frames in.
    ExecuteResult result;
    if (light_paths.empty()) return result;

    // Surface a deferred QE-load failure from the constructor before
    // any per-frame work. ok=false here means the engine isn't usable
    // for this batch and the module/UI can show the error to the user.
    if (!qe_load_error_.empty()) {
        obs.message(qe_load_error_);
        ExecuteResult err{};
        err.ok    = false;
        err.error = qe_load_error_;
        return err;
    }

    // ═══ SETUP ═══════════════════════════════════════════════════════

    // Read first frame to determine dimensions and channel config
    auto first = FITSReader::read(light_paths[0]);
    if (!first.success) return result;

    int raw_width = first.image.width();
    int raw_height = first.image.height();

    // Classify the first frame's filter (drives initial ChannelConfig).
    Filter first_filter = filter_classifier_->classify(first.metadata);
    BayerPattern bayer = parse_bayer_pattern(first.metadata.bayer_pattern);

    // Loud-fail: an unknown FILTER on Bayer data is a batch-stopper per
    // spec §6.3 — silently treating it as plain OSC would corrupt the
    // routing and silently mis-color downstream output.
    if (first_filter.cls == FilterClass::UNKNOWN && bayer != BayerPattern::NONE) {
        ExecuteResult err{};
        err.ok    = false;
        err.error = "FILTER='" + first_filter.name + "' on Bayer frame not in QE DB. " +
                    "Add it to ~/.nukex4/qe_overrides.json (see docs) and retry, " +
                    "or remove the FILTER keyword to default to plain OSC.";
        obs.message(err.error);
        return err;
    }

    // Surface mono-side warning if the classifier emitted one (e.g. the
    // mono-with-FILTER guesses we accept silently but want logged).
    if (!filter_classifier_->last_warning().empty()) {
        obs.message(filter_classifier_->last_warning());
    }

    ChannelConfig ch_config = ChannelConfig::from_filter(first_filter);
    if (bayer != BayerPattern::NONE) {
        ch_config.bayer = bayer; // FITS header wins over the from_filter() default
    }

    // After debayer, dimensions may change for Bayer data.
    // Bilinear debayer produces same-size output.
    int out_width = raw_width;
    int out_height = raw_height;
    int n_ch = ch_config.n_channels;

    // Build master flat if flats provided
    Image master_flat;
    if (!flat_paths.empty()) {
        master_flat = FlatCalibration::build_master_flat(flat_paths);
    }

    // Allocate cube
    Cube cube(out_width, out_height, ch_config);

    // Allocate frame cache
    int n_frames = static_cast<int>(light_paths.size());
    FrameCache cache(out_width, out_height, n_ch, n_frames, config_.cache_dir);

    // Frame-level metadata
    std::vector<FrameStats> frame_stats(n_frames);
    std::vector<float> frame_fwhms(n_frames, 0.0f);

    // Initialize aligner
    FrameAligner aligner(config_.aligner_config);

    // ═══ PHASE A — Streaming Accumulation ════════════════════════════

    obs.message("Frames: " + std::to_string(n_frames) + " light"
                + (flat_paths.empty() ? "" : ", " + std::to_string(flat_paths.size()) + " flat"));
    obs.begin_phase("Phase A: Loading frames", n_frames);

    // Helper: route one (channel-name → value) sample into a voxel slot.
    // Initializes the histogram range on the very first sample for the slot
    // and then performs the standard Welford + histogram update. Slot
    // indexing is via the cube's (possibly merged) channel_config; an
    // unrecognized name aborts because the routing dispatch upstream is
    // expected to have already validated it.
    auto route_sample = [&](SubcubeVoxel& voxel,
                            const std::string& slot_name,
                            float value) {
        int idx = cube.channel_config.slot_index(slot_name);
        if (idx < 0) {
            // Routing tried to push a sample into a slot that was never
            // allocated by ChannelConfig::merge() — that is a programmer
            // error in the dispatch table. Aborting beats corrupting an
            // adjacent slot's accumulator.
            std::abort();
        }
        voxel.welford[idx].update(value);
        if (voxel.welford[idx].count() == 1) {
            voxel.histogram[idx].initialize_range(value - 0.1f, value + 0.1f);
        }
        voxel.histogram[idx].update(value);
    };

    for (int f = 0; f < n_frames; f++) {
        std::string filename = light_paths[f];
        auto slash = filename.rfind('/');
        if (slash != std::string::npos) filename = filename.substr(slash + 1);
        obs.advance(0, "Frame " + std::to_string(f + 1) + "/" + std::to_string(n_frames)
                       + ": " + filename);

        // 1. Load
        auto read_result = (f == 0) ? std::move(first) : FITSReader::read(light_paths[f]);
        if (!read_result.success) {
            obs.advance(1, "  skipped (read failed)");
            continue;
        }

        Image image = std::move(read_result.image);
        auto& meta = read_result.metadata;

        // 1b. Per-frame filter classification — drives Phase A routing.
        Filter frame_filter = filter_classifier_->classify(meta);

        // Mid-batch unknown-on-Bayer is rejected per-frame (not the whole
        // batch — only the first frame's UNKNOWN-on-Bayer is fatal).
        bool frame_is_bayer = parse_bayer_pattern(meta.bayer_pattern) != BayerPattern::NONE;
        if (frame_filter.cls == FilterClass::UNKNOWN && frame_is_bayer) {
            obs.advance(1, "  skipped — unknown FILTER='" + frame_filter.name +
                           "' on Bayer frame (add to qe_overrides.json to recover)");
            result.n_frames_failed_alignment++;
            continue;
        }

        // Surface mono-side warnings for downstream visibility.
        if (!filter_classifier_->last_warning().empty()) {
            obs.advance(0, std::string("  ") + filter_classifier_->last_warning());
        }

        // Merge per-frame config into the cube's running channel_config.
        // Idempotent if the per-frame filter matches the running config.
        ChannelConfig per_frame_cfg = ChannelConfig::from_filter(frame_filter);
        cube.channel_config = ChannelConfig::merge(cube.channel_config, per_frame_cfg);

        // 2. Debayer
        if (bayer != BayerPattern::NONE) {
            obs.advance(0, "  debayering (" + meta.bayer_pattern + ")");
            image = DebayerEngine::debayer(image, bayer);
        }

        // 3. Flat correct
        if (!master_flat.empty()) {
            obs.advance(0, "  flat correcting");
            FlatCalibration::apply(image, master_flat);
        }

        // 4. Align
        // Pre-check saturation so a blown-out frame gets a specific log line
        // ("SKIPPED — blown out, X%") rather than the generic
        // "aligned: FAILED (stars=0)".  The actual guard that keeps
        // StarDetector fast lives in StarDetector::detect — this is purely
        // for log clarity in the Process Console.
        float sat_frac = StarDetector::saturation_fraction(
            image, config_.aligner_config.star_config.saturation_level);
        bool blown_out =
            sat_frac >= config_.aligner_config.star_config.saturation_reject_fraction;

        obs.advance(0, "  aligning");
        auto aligned = aligner.align(image, f);
        // Surface the alignment outcome including the actual inlier / RMS
        // numbers so a user reading the Process Console can tell which
        // frames genuinely aligned from which were weight-penalised.
        // (The previous log said "aligned (N stars)" regardless of success,
        // which hid 61-failed-of-65 alignment-quality regressions for an
        // entire release cycle.)
        if (blown_out) {
            char pct[16];
            std::snprintf(pct, sizeof(pct), "%.1f", sat_frac * 100.0f);
            obs.advance(0,
                std::string("  aligned: SKIPPED (blown out — ") + pct
                + "% pixels at saturation)");
        } else {
           const auto& a = aligned.alignment;
           std::string status = a.alignment_failed ? "FAILED" : "ok";
           std::string rms_str;
           {
              char buf[32];
              std::snprintf(buf, sizeof(buf), "%.3f", a.match.rms_error);
              rms_str = buf;
           }
           obs.advance(0, "  aligned: " + status
                          + " (stars=" + std::to_string(aligned.stars.stars.size())
                          + ", inliers=" + std::to_string(a.match.n_inliers)
                          + ", rms=" + rms_str + " px"
                          + (a.is_meridian_flipped ? ", meridian-flipped" : "")
                          + ")");
        }

        // 5. Cache aligned frame
        obs.advance(0, "  caching");
        cache.write_frame(f, aligned.image);

        // 6. Frame-level stats
        float frame_median = compute_frame_median(aligned.image);
        float frame_fwhm = compute_median_fwhm(aligned.stars);

        frame_stats[f].read_noise = meta.read_noise;
        frame_stats[f].gain = meta.gain;
        frame_stats[f].exposure = meta.exposure;
        frame_stats[f].has_noise_keywords = meta.has_noise_keywords;
        frame_stats[f].is_meridian_flipped = aligned.alignment.is_meridian_flipped;
        frame_stats[f].frame_weight = aligned.alignment.weight_penalty;
        frame_stats[f].median_luminance = frame_median;
        frame_stats[f].fwhm = frame_fwhm;
        frame_fwhms[f] = frame_fwhm;

        if (aligned.alignment.alignment_failed) {
            result.n_frames_failed_alignment++;
        }

        // 7. Accumulate into cube via Phase A router.
        //
        // The classifier on the per-frame metadata picked one of the five
        // FilterClass cases below; UNKNOWN was already rejected upstream.
        // Each case dispatches the appropriate per-pixel value(s) into
        // named voxel slots via the shared route_sample helper.
        //
        // BROADBAND_OSC additionally synthesises an "L" slot via rec709
        // luminance (0.299 R + 0.587 G + 0.114 B) — that's how we get the
        // "OSC-as-LRGB" effect that the spec promises.
        obs.advance(0, "  accumulating");
        switch (frame_filter.cls) {
            case FilterClass::BROADBAND_OSC: {
                for (int y = 0; y < out_height; y++) {
                    for (int x = 0; x < out_width; x++) {
                        auto& voxel = cube.at(x, y);
                        float r = aligned.image.at(x, y, 0);
                        float g = aligned.image.at(x, y, 1);
                        float b = aligned.image.at(x, y, 2);
                        float l = 0.299f * r + 0.587f * g + 0.114f * b;
                        route_sample(voxel, "R", r);
                        route_sample(voxel, "G", g);
                        route_sample(voxel, "B", b);
                        route_sample(voxel, "L", l);
                        voxel.n_frames++;
                    }
                }
                break;
            }
            case FilterClass::DUAL_NB_OSC: {
                std::string r_slot = "R_" + frame_filter.name;
                std::string g_slot = "G_" + frame_filter.name;
                std::string b_slot = "B_" + frame_filter.name;
                for (int y = 0; y < out_height; y++) {
                    for (int x = 0; x < out_width; x++) {
                        auto& voxel = cube.at(x, y);
                        route_sample(voxel, r_slot, aligned.image.at(x, y, 0));
                        route_sample(voxel, g_slot, aligned.image.at(x, y, 1));
                        route_sample(voxel, b_slot, aligned.image.at(x, y, 2));
                        voxel.n_frames++;
                    }
                }
                break;
            }
            case FilterClass::BROADBAND_L:
            case FilterClass::NARROWBAND_SINGLE:
            case FilterClass::BROADBAND_RGB: {
                // Mono frames: channel 0 of the (post-debayer) image. The
                // slot name comes from the filter — "L" for broadband-L,
                // "Ha"/"OIII"/"SII" for narrowband-single, "R"/"G"/"B"
                // for an explicit R/G/B mono filter.
                const std::string& slot_name = frame_filter.name;
                for (int y = 0; y < out_height; y++) {
                    for (int x = 0; x < out_width; x++) {
                        auto& voxel = cube.at(x, y);
                        route_sample(voxel, slot_name, aligned.image.at(x, y, 0));
                        voxel.n_frames++;
                    }
                }
                break;
            }
            case FilterClass::UNKNOWN:
                // Already handled at the top of the per-frame loop —
                // reaching here means the upstream gate is broken.
                std::abort();
        }

        cube.n_frames_loaded++;
        result.n_frames_processed++;
        obs.advance(1);

        if (obs.is_cancelled()) {
            obs.message("Cancelled during frame loading.");
            obs.end_phase();
            return result;
        }
    }

    obs.end_phase();

    if (result.n_frames_processed == 0) return result;

    // ═══ BETWEEN PHASES — Global Statistics ══════════════════════════

    // Phase A may have merged additional slots into cube.channel_config
    // (e.g. mixed L + HaO3 batch grew it from 1→4). Phase B / C and the
    // output images need to track that merged total — keep n_ch in sync
    // with the cube's current channel count.
    n_ch = cube.channel_config.n_channels;

    // Compute fwhm_best and backfill PSF weights
    float fwhm_best = 1e30f;
    for (int f = 0; f < n_frames; f++) {
        if (frame_fwhms[f] > 0.0f) {
            fwhm_best = std::min(fwhm_best, frame_fwhms[f]);
        }
    }
    if (fwhm_best < 1e-10f || fwhm_best > 1e20f) fwhm_best = 1.0f;

    for (int f = 0; f < n_frames; f++) {
        if (frame_fwhms[f] > 0.0f) {
            float ratio = frame_fwhms[f] / fwhm_best;
            frame_stats[f].psf_weight = std::exp(
                -0.5f * (ratio - 1.0f) * (ratio - 1.0f) / 0.25f);
        } else {
            frame_stats[f].psf_weight = 1.0f;
        }
    }

    // Compute frame-level cloud scores using median-of-medians as reference
    {
        std::vector<float> medians;
        medians.reserve(n_frames);
        for (int f = 0; f < n_frames; f++) {
            if (frame_stats[f].median_luminance > 0.0f) {
                medians.push_back(frame_stats[f].median_luminance);
            }
        }
        float median_of_medians = 0.0f;
        if (!medians.empty()) {
            median_of_medians = median_inplace(medians.data(),
                                               static_cast<int>(medians.size()));
        }
        WeightConfig wc_defaults;  // for cloud_threshold / cloud_penalty
        for (int f = 0; f < n_frames; f++) {
            if (median_of_medians > 1e-30f &&
                frame_stats[f].median_luminance < wc_defaults.cloud_threshold * median_of_medians) {
                frame_stats[f].cloud_score = wc_defaults.cloud_penalty;
            } else {
                frame_stats[f].cloud_score = 1.0f;
            }
        }
    }

    // ═══ PHASE B — Analysis (GPU-accelerated) ════════════════════════

    ModelSelector fitter(config_.fitting_config);

    // Output images
    Image stacked(out_width, out_height, n_ch);
    Image noise_map(out_width, out_height, n_ch);

    // GPU executor handles: weight computation, robust stats, pixel selection.
    // Distribution fitting stays on CPU (Ceres Solver).
    GPUExecutor gpu(config_.gpu_config);

    if (gpu.active_backend() == GPUBackend::OPENCL) {
        const auto& di = gpu.device_info();
        obs.message("GPU: " + di.name + " (OpenCL, "
                    + std::to_string(di.global_mem_bytes / (1024*1024)) + " MB)");
    } else {
        obs.message("GPU: CPU fallback");
    }

    // Fitting callback — called per-voxel by the GPU executor after
    // kernels 1+2 complete. Runs the Ceres-based model selection cascade.
    auto fitting_fn = [&fitter](SubcubeVoxel& voxel,
                                 const float* values, const float* weights,
                                 int nf, int nc,
                                 const FrameStats* /*fs*/) {
        for (int ch = 0; ch < nc; ch++) {
            fitter.select(values + ch * nf, weights + ch * nf, nf, voxel, ch);
        }
    };

    auto phase_b_start = std::chrono::steady_clock::now();
    gpu.execute_phase_b(cube, cache, frame_stats, config_.weight_config,
                         fitting_fn, stacked, noise_map, &obs);
    auto phase_b_end = std::chrono::steady_clock::now();
    long phase_b_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          phase_b_end - phase_b_start ).count();
    obs.message("Phase B: " + std::to_string(phase_b_ms) + " ms");

    if (obs.is_cancelled()) {
        obs.message("Cancelled during distribution fitting.");
        return result;
    }

    // Post-processing: dominant shape + quality scores + spatial context
    obs.begin_phase("Phase C: Post-processing", 3);

    obs.advance(1, "dominant shape computation");
    for (int y = 0; y < out_height; y++) {
        for (int x = 0; x < out_width; x++) {
            auto& voxel = cube.at(x, y);
            compute_dominant_shape(voxel, n_ch);
        }
    }

    obs.advance(1, "quality scores");
    for (int y = 0; y < out_height; y++) {
        for (int x = 0; x < out_width; x++) {
            auto& voxel = cube.at(x, y);
            float avg_snr = 0.0f;
            for (int ch = 0; ch < n_ch; ch++) avg_snr += voxel.snr[ch];
            avg_snr /= n_ch;
            float cloud_fraction = (voxel.n_frames > 0) ?
                static_cast<float>(voxel.cloud_frame_count) / voxel.n_frames : 0.0f;
            voxel.quality_score = voxel.distribution[0].confidence
                * (1.0f - cloud_fraction)
                * std::min(1.0f, avg_snr / 50.0f);
            voxel.confidence = voxel.distribution[0].confidence;
        }
    }

    // Spatial context (GPU kernel 4)
    obs.advance(0, "spatial context");
    gpu.execute_spatial_context(stacked, cube, &obs);
    obs.advance(1);

    obs.end_phase();

    // Quality map
    Image quality_map = OutputAssembler::assemble_quality_map(cube);

    result.stacked = std::move(stacked);
    result.noise_map = std::move(noise_map);
    result.quality_map = std::move(quality_map);
    // Move the cube into a heap-allocated holder on the result so
    // Phase B (Task 10) and the integration tests can read derived
    // slot indices and per-voxel accumulators after execute() returns.
    // (unique_ptr because Cube is forward-declared in the public
    //  header — see stacking_engine.hpp namespace comment.)
    result.cube = std::make_unique<Cube>(std::move(cube));

    return result;
}

} // namespace nukex
