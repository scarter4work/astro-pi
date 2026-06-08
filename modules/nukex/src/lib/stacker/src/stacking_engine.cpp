#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/stacker/cache_sig.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/core/filter.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/io/flat_calibration.hpp"
// TASK-14-COLLAPSE: this include exists so we can dereference the
// pImpl unique_ptr<FilterClassifier>. When the v4 module-level
// FilterClass enum is deleted, the pImpl can revert to a value
// member and this include can move into the header.
#include "nukex/io/filter_classifier.hpp"
#include "nukex/alignment/frame_aligner.hpp"
// TASK-14-COLLAPSE: same — pImpl-only include, can move to the
// header once the namespace collision is gone.
#include "nukex/calibration/qe_database.hpp"
// TASK-14-COLLAPSE: same — for ChannelDecomposer pImpl.
#include "nukex/calibration/channel_decomposer.hpp"
#include <Eigen/Dense>
#include "nukex/classify/weight_computer.hpp"
// ColorComposer is module-owned (Task 11 / Task 12). The engine produces
// structured DerivedStack output; the module composes it for display.
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
#include <map>
#include <numeric>
#include <set>
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
}

// TASK-14-COLLAPSE: out-of-line so the unique_ptr<FilterClassifier> /
// <QEDatabase> / <ChannelDecomposer> destructors can see the full types
// from this TU's includes. When the v4 module enum goes away, the
// engine members can become value-typed and the destructor can revert
// to `= default;` in the header.
StackingEngine::~StackingEngine() = default;

// TASK-14-COLLAPSE: ExecuteResult special members are out-of-line for
// the same forward-decl reason — std::unique_ptr<Cube> needs the
// complete Cube type at the destructor / move-ctor / move-assign
// instantiation. Once Cube can be a value member, all of these can be
// `= default;` in the header.
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

    // Task 10A: one FrameCache per (width, height, n_ch) signature.
    //
    // Pre-Task-10A, a single FrameCache was allocated up-front based on the
    // first frame's Bayer mode. That broke in two ways:
    //   1. Mixed mono+Bayer batches OOB'd at write_frame() when frame
    //      geometry differed from the first frame's cache dimensions.
    //   2. BROADBAND_OSC's synthesised L slot (cube ch=3) read garbage from
    //      a 3-channel cache during Phase B — ch=3 is out of range.
    // Both stem from v4's implicit cube_n_ch == cache_n_ch == debayer_n_ch
    // assumption, which v5's slot semantics broke.
    //
    // Fix: caches is a map keyed on (w, h, n_ch). Each frame is written to
    // the cache matching its post-debayer geometry. Phase B reads through
    // slot_cache_refs (built below after Phase A) instead of indexing the
    // cache directly by cube slot index.
    int n_frames = static_cast<int>(light_paths.size());
    std::map<CacheSig, FrameCache> caches;
    auto get_or_create_cache = [&](int w, int h, int n_ch) -> FrameCache& {
        CacheSig sig{w, h, n_ch};
        auto it = caches.find(sig);
        if (it == caches.end()) {
            it = caches.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(sig),
                std::forward_as_tuple(w, h, n_ch, n_frames, config_.cache_dir)
            ).first;
        }
        return it->second;
    };

    // Camera names of frames that contributed to a DUAL_NB_OSC slot. Used to
    // gate the Q-solve at Phase B end: per-camera Q matrices are not yet
    // implemented, so a multi-camera dual-NB stack would silently apply
    // camera A's Q to camera B's data (wrong color). One camera per dual-NB
    // batch is currently the contract; mixed-camera support is a Task 11+
    // follow-up. Broadband / single-line slots don't go through Q-solve and
    // therefore don't care, so we only track this for DUAL_NB_OSC frames.
    std::set<std::string> dual_nb_cameras;

    // Frame-level metadata
    std::vector<FrameStats> frame_stats(n_frames);
    std::vector<float> frame_fwhms(n_frames, 0.0f);

    // Initialize aligner
    FrameAligner aligner(config_.aligner_config);

    // ═══ PHASE A — Streaming Accumulation ════════════════════════════

    obs.message("Frames: " + std::to_string(n_frames) + " light"
                + (flat_paths.empty() ? "" : ", " + std::to_string(flat_paths.size()) + " flat"));
    obs.begin_phase("Phase A: Loading frames", n_frames);

    // Helper: route one pre-resolved-index sample into a voxel slot.
    // Initializes the histogram range on the very first sample for the
    // slot and then performs the standard Welford + histogram update.
    //
    // Index-keyed variant is the *only* variant we ship — name→index
    // resolution is hoisted to once-per-frame at each per-frame switch
    // case below. Pre-fix this was a string-keyed lookup hit per pixel
    // per slot (96M lookups/frame at 6000×4000 OSC, ~600ms/stack
    // overhead). Caller is expected to bounds-check the index up-front
    // (the per-frame switch does this and std::abort()s on missing
    // slots — that is a programmer error in the dispatch table /
    // ChannelConfig::merge(), not a runtime input failure).
    auto route_sample_idx = [&](SubcubeVoxel& voxel,
                                int idx,
                                float value) {
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
            // Filter rejection: the frame was skipped because its FILTER
            // keyword is not present in the QE database. This is not an
            // alignment failure — the aligner was never invoked. Track it
            // separately so the user-facing summary can distinguish
            // "your scope wandered" from "your filter wheel reported an
            // unknown name." (Task 12 counter split.)
            result.n_frames_rejected_filter++;
            continue;
        }

        // Surface mono-side warnings for downstream visibility.
        if (!filter_classifier_->last_warning().empty()) {
            obs.advance(0, std::string("  ") + filter_classifier_->last_warning());
        }

        // Track DUAL_NB_OSC cameras for the Q-solve mixed-camera guard.
        if (frame_filter.cls == FilterClass::DUAL_NB_OSC) {
            dual_nb_cameras.insert(frame_filter.camera);
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

        // 5. Cache aligned frame into the geometry-matched cache.
        obs.advance(0, "  caching");
        get_or_create_cache(aligned.image.width(),
                            aligned.image.height(),
                            aligned.image.n_channels()).write_frame(f, aligned.image);

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
            // Real alignment failure: the Groth triangle matcher ran but
            // could not find a valid transform (too few stars, bad RMS,
            // etc.). The frame is stacked with a 0.5x weight penalty.
            // This counter is strictly alignment misses; filter rejections
            // are counted separately in n_frames_rejected_filter.
            result.n_frames_failed_alignment++;
        }

        // 7. Accumulate into cube via Phase A router.
        //
        // The classifier on the per-frame metadata picked one of the five
        // FilterClass cases below; UNKNOWN was already rejected upstream.
        // Each case dispatches the appropriate per-pixel value(s) into
        // named voxel slots via the shared route_sample_idx helper.
        //
        // BROADBAND_OSC additionally synthesises an "L" slot via rec709
        // luminance (0.299 R + 0.587 G + 0.114 B) — that's how we get the
        // "OSC-as-LRGB" effect that the spec promises.
        obs.advance(0, "  accumulating");
        switch (frame_filter.cls) {
            case FilterClass::BROADBAND_OSC: {
                // Pre-resolve once per frame — not inside the pixel loop.
                // At 6000×4000 OSC a string-keyed slot_index() call per
                // pixel per slot costs ~600ms/stack (96M lookups). Resolving
                // up-front reduces that to 4 calls per frame. Any -1 result
                // means ChannelConfig::merge() produced an incomplete slot
                // table — that is a programmer error, so we std::abort()
                // before entering the loop rather than silently routing into
                // a garbage index.
                int r_idx = cube.channel_config.slot_index("R");
                int g_idx = cube.channel_config.slot_index("G");
                int b_idx = cube.channel_config.slot_index("B");
                int l_idx = cube.channel_config.slot_index("L");
                if (r_idx < 0 || g_idx < 0 || b_idx < 0 || l_idx < 0) {
                    std::abort(); // missing BROADBAND_OSC slot — dispatch-table bug
                }
                for (int y = 0; y < out_height; y++) {
                    for (int x = 0; x < out_width; x++) {
                        auto& voxel = cube.at(x, y);
                        float r = aligned.image.at(x, y, 0);
                        float g = aligned.image.at(x, y, 1);
                        float b = aligned.image.at(x, y, 2);
                        float l = 0.299f * r + 0.587f * g + 0.114f * b;
                        route_sample_idx(voxel, r_idx, r);
                        route_sample_idx(voxel, g_idx, g);
                        route_sample_idx(voxel, b_idx, b);
                        route_sample_idx(voxel, l_idx, l);
                        voxel.n_frames++;
                    }
                }
                break;
            }
            case FilterClass::DUAL_NB_OSC: {
                std::string r_slot = "R_" + frame_filter.name;
                std::string g_slot = "G_" + frame_filter.name;
                std::string b_slot = "B_" + frame_filter.name;
                // Same once-per-frame hoist as BROADBAND_OSC: 3 lookups
                // instead of 3 × width × height per-pixel string lookups.
                // Abort on -1 — a missing dual-NB slot is a merge() bug,
                // not a recoverable runtime condition.
                int r_idx = cube.channel_config.slot_index(r_slot);
                int g_idx = cube.channel_config.slot_index(g_slot);
                int b_idx = cube.channel_config.slot_index(b_slot);
                if (r_idx < 0 || g_idx < 0 || b_idx < 0) {
                    std::abort(); // missing DUAL_NB_OSC slot — dispatch-table bug
                }
                for (int y = 0; y < out_height; y++) {
                    for (int x = 0; x < out_width; x++) {
                        auto& voxel = cube.at(x, y);
                        route_sample_idx(voxel, r_idx, aligned.image.at(x, y, 0));
                        route_sample_idx(voxel, g_idx, aligned.image.at(x, y, 1));
                        route_sample_idx(voxel, b_idx, aligned.image.at(x, y, 2));
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
                // One lookup per frame instead of one per pixel — same
                // rationale as the OSC cases above. Abort if the slot is
                // absent: it means merge() didn't register this filter's
                // slot, which is a dispatch-table programmer error.
                int slot_idx = cube.channel_config.slot_index(slot_name);
                if (slot_idx < 0) {
                    std::abort(); // missing mono slot — dispatch-table bug
                }
                for (int y = 0; y < out_height; y++) {
                    for (int x = 0; x < out_width; x++) {
                        auto& voxel = cube.at(x, y);
                        route_sample_idx(voxel, slot_idx, aligned.image.at(x, y, 0));
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
    // Single-source-of-truth slot count for the rest of execute(). Future
    // edits between here and Phase B that re-touch `merge()` should update
    // this value rather than reading channel_config.n_channels three times
    // (the slot_cache_refs sizing + routing loop both use this).
    const int n_slots = n_ch;

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

    // ── Build slot → cache routing table for Phase B ─────────────────
    //
    // For each cube slot, determine which FrameCache and channel index (or
    // synthesis rule) Phase B should use to retrieve per-frame pixel values.
    //
    // The routing is explicit and verbose here by design: encoding
    // (FilterClass × cache geometry) rules in a flat switch is clearer than
    // smearing them across ChannelConfig string conventions or slot-name
    // parsing inside the GPU executor. Every (FilterClass × cache geometry)
    // combination must be covered; missing coverage falls through to the
    // cache=nullptr sentinel which makes Phase B use welford-only stats.
    std::vector<ChannelCacheRef> slot_cache_refs(n_slots);

    // Grab commonly needed cache pointers once — avoids re-scanning the map
    // per slot.
    const FrameCache* cache_3ch = nullptr; // first 3-channel (OSC) cache
    const FrameCache* cache_1ch = nullptr; // first 1-channel (mono) cache
    for (auto& [sig, c] : caches) {
        if (std::get<2>(sig) == 3 && cache_3ch == nullptr) cache_3ch = &c;
        if (std::get<2>(sig) == 1 && cache_1ch == nullptr) cache_1ch = &c;
    }

    // Up-front count of mono R/G/B slots in this cube — needed to
    // distinguish a single-color mono batch (safe to route slot 0 → cache
    // ch 0) from a mixed mono-R + mono-G + mono-B batch (where the std::map
    // collapses three same-shape (W,H,1) caches into one; routing all three
    // slots to ch=0 would silently mix per-frame R/G/B values across slots).
    // TASK-11-MONO-RGB will add per-filter cache tracking so the mixed case
    // can use full Phase B stats; until then we leave cache=nullptr in that
    // case so distribution fitting falls back loud-fail to welford-only.
    int rgb_mono_slot_count = 0;
    for (int j = 0; j < n_slots; ++j) {
        const std::string& nm = cube.channel_config.slot_name(j);
        if (nm == "R" || nm == "G" || nm == "B") ++rgb_mono_slot_count;
    }

    for (int slot_i = 0; slot_i < n_slots; ++slot_i) {
        const std::string& name = cube.channel_config.slot_name(slot_i);

        if (name == "L") {
            // Two cases: raw mono-L filter (1ch cache) or BROADBAND_OSC
            // synthesised L (3ch cache, no raw L in cache).
            if (cache_1ch != nullptr) {
                // Raw L filter: direct read from channel 0 of the mono cache.
                slot_cache_refs[slot_i] = {cache_1ch, 0, SlotSynthesis::DIRECT};
            } else if (cache_3ch != nullptr) {
                // BROADBAND_OSC synthesised L: derive 0.299R+0.587G+0.114B at
                // read time. Same formula as Phase A's voxel accumulation — keeps
                // the distribution fitting consistent with the Welford stats.
                slot_cache_refs[slot_i] = {cache_3ch, -1, SlotSynthesis::REC709_LUMA};
            }
            // If neither cache exists the slot stays at the default (nullptr),
            // and Phase B falls back to welford-only for this slot.

        } else if (name == "R" || name == "G" || name == "B") {
            // BROADBAND_OSC raw R/G/B OR an explicit mono R/G/B filter.
            // Prefer the 3ch (OSC) cache; fall back to the mono cache if only
            // mono R/G/B filter frames were fed (BROADBAND_RGB batch).
            int cache_ch_target = (name == "R") ? 0 : (name == "G") ? 1 : 2;
            if (cache_3ch != nullptr) {
                slot_cache_refs[slot_i] = {cache_3ch, cache_ch_target, SlotSynthesis::DIRECT};
            } else if (cache_1ch != nullptr && rgb_mono_slot_count <= 1) {
                // Single-color mono batch (only one of R/G/B present in the
                // cube): the lone 1ch cache carries that color, slot 0 →
                // cache ch 0. Safe.
                slot_cache_refs[slot_i] = {cache_1ch, 0, SlotSynthesis::DIRECT};
            }
            // TASK-11-MONO-RGB: mixed mono-R + mono-G + mono-B batches collapse
            // into a single (W,H,1) cache via std::map<CacheSig,...>; routing
            // all three slots to ch=0 of that cache would mix per-frame values.
            // Leaving cache=nullptr here forces Phase B to use welford-only
            // stats for these slots — correct (if coarser) until per-filter
            // cache tracking lands.

        } else if (name.size() >= 2 &&
                   (name[0] == 'R' || name[0] == 'G' || name[0] == 'B') &&
                   name[1] == '_') {
            // DUAL_NB_OSC: slot names like "R_HaO3", "G_HaO3", "B_S2O3".
            // Frames were debayered into a 3-channel image; cache is 3ch.
            if (cache_3ch != nullptr) {
                int cache_ch_target = (name[0] == 'R') ? 0 : (name[0] == 'G') ? 1 : 2;
                slot_cache_refs[slot_i] = {cache_3ch, cache_ch_target, SlotSynthesis::DIRECT};
            }
            // No 3ch cache for DUAL_NB is a classifier guard violation —
            // fall through with cache=nullptr (welford-only Phase B).

        } else {
            // NARROWBAND_SINGLE: "Ha", "OIII", "SII", or any future mono slot.
            // These come from mono frames → 1ch cache, channel 0.
            if (cache_1ch != nullptr) {
                slot_cache_refs[slot_i] = {cache_1ch, 0, SlotSynthesis::DIRECT};
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

    // All caches are written in lockstep (one frame per cache per iteration),
    // so any cache's n_frames_written() gives the correct count for Phase B.
    int n_frames_written = caches.empty() ? 0
                         : caches.begin()->second.n_frames_written();

    auto phase_b_start = std::chrono::steady_clock::now();
    gpu.execute_phase_b(cube, slot_cache_refs, n_frames_written,
                        frame_stats, config_.weight_config,
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

    // ═══ PHASE B FOLLOW-UP — Q-solve derived semantic slots ═══════════
    //
    // PixelSelector wrote the per-pixel best raw value for each cube slot
    // into `stacked` (one image channel per slot). For dual-narrowband
    // groups (HaO3 = Ha + OIII; S2O3 = SII + OIII) we now decompose the
    // raw R/G/B columns into emission-line components via the camera-and-
    // filter-specific Q matrix. Broadband + single-line slots pass through
    // unchanged. The result lives in DerivedStack which downstream
    // (NukeXInstance's ColorComposer at display time, file output) reads
    // from instead of `stacked`.
    //
    // Sample-count weighted mean is used for multi-source merge so that
    // frames with more samples contribute proportionally — the simpler
    // arithmetic mean would let a 5-frame S2O3 batch wash out a 50-frame
    // HaO3 batch's OIII contribution.
    {
        DerivedStack derived;
        derived.width  = out_width;
        derived.height = out_height;
        const int N = out_width * out_height;

        // Helper: copy a stacked-image slot to derived (broadband passthrough).
        // Returns immediately if the slot is absent — does NOT insert an empty
        // entry in derived.slots (callers must null-check on lookup).
        auto passthrough_slot = [&](const std::string& name) {
            int idx = cube.channel_config.slot_index(name);
            if (idx < 0) return;
            auto& dst = derived.slots[name];
            dst.assign(N, 0.0f);
            for (int y = 0; y < out_height; ++y) {
                for (int x = 0; x < out_width; ++x) {
                    dst[y * out_width + x] = stacked.at(x, y, idx);
                }
            }
        };
        for (const char* name : {"L", "R", "G", "B", "Ha", "OIII", "SII"}) {
            passthrough_slot(name);
        }

        // Discover dual-NB groups present in this batch.
        struct QGroup {
            std::string filter_name;  // "HaO3", "S2O3", ...
            std::string r_slot, g_slot, b_slot;
        };
        std::vector<QGroup> groups;
        for (const char* fname_cstr : {"HaO3", "S2O3"}) {
            const std::string fname(fname_cstr);
            if (cube.channel_config.slot_index("R_" + fname) >= 0) {
                groups.push_back({fname, "R_" + fname, "G_" + fname, "B_" + fname});
            }
        }

        // Mixed-camera guard. Q matrices are camera-specific (built from each
        // camera's QE curve); applying camera A's Q to camera B's per-pixel
        // values produces silently miscolored derived emission slots. Until
        // per-frame Q dispatch lands (Task 11+), the contract is one camera
        // per dual-NB batch. If the user fed dual-NB frames from multiple
        // cameras we loud-fail here rather than emit garbage downstream.
        // Broadband and single-line slots don't go through Q-solve so they
        // pass through unchanged regardless.
        std::string q_build_camera;
        if (!groups.empty()) {
            if (dual_nb_cameras.size() == 1) {
                q_build_camera = *dual_nb_cameras.begin();
            } else if (dual_nb_cameras.size() > 1) {
                std::string list;
                for (const auto& cam : dual_nb_cameras) {
                    if (!list.empty()) list += ", ";
                    list += cam;
                }
                result.ok    = false;
                result.error = "Phase B Q-solve: dual-NB frames span multiple "
                               "cameras (" + list + "). Per-camera Q dispatch is "
                               "not yet implemented; stack each camera separately, "
                               "or limit the batch to one camera.";
                return result;
            }
        }

        // Multi-source line accumulators. Keyed by emission-line name (e.g. "OIII")
        // since it can be contributed by both HaO3 and S2O3.
        std::unordered_map<std::string, std::vector<double>> line_sum;
        std::unordered_map<std::string, std::vector<int64_t>> line_n;
        auto ensure_acc = [&](const std::string& name) {
            if (!line_sum.count(name)) {
                line_sum[name].assign(N, 0.0);
                line_n  [name].assign(N, 0);
            }
        };

        for (const auto& g : groups) {
            int ri = cube.channel_config.slot_index(g.r_slot);
            int gi = cube.channel_config.slot_index(g.g_slot);
            int bi = cube.channel_config.slot_index(g.b_slot);

            FilterPassband fp = qe_database_->lookup_filter(g.filter_name);

            Eigen::MatrixXd Q;
            try {
                // Camera comes from `dual_nb_cameras` (validated above as a
                // single-element set). Using `first_filter.camera` directly
                // would silently misbuild Q if the first frame happened to be
                // a broadband single-line under a different camera than the
                // dual-NB frames.
                Q = decomposer_->build_q(q_build_camera, g.filter_name);
            } catch (const std::runtime_error& e) {
                // ChannelDecomposer::build_q can throw any of three:
                //   - UnknownCameraError  (camera not in QE DB; e.g. user
                //     stacked data from a camera not yet in share/qe_database.json
                //     and didn't supply a qe_overrides.json entry)
                //   - UnknownFilterError  (filter name not in QE DB)
                //   - SingularQError      (Q is rank-deficient — degenerate QE)
                // All three are user-actionable: they indicate a missing
                // entry in the QE DB or override file. Loud-fail with the
                // descriptive message lets the module surface a clear
                // remediation hint rather than terminating the process on
                // an uncaught exception.
                result.ok    = false;
                result.error = std::string("Phase B Q-solve: ") + e.what();
                return result;
            }
            auto qr = Q.colPivHouseholderQr();

            for (int y = 0; y < out_height; ++y) {
                for (int x = 0; x < out_width; ++x) {
                    const int p = y * out_width + x;
                    Eigen::Vector3d rgb(stacked.at(x, y, ri),
                                        stacked.at(x, y, gi),
                                        stacked.at(x, y, bi));
                    Eigen::VectorXd lines = qr.solve(rgb);
                    // Sample count is the MIN across R/G/B welford counters.
                    // For DUAL_NB_OSC the Phase A router writes all three in
                    // lockstep (frame-by-frame), so today the three counters
                    // match exactly. min() is a defensive lower bound: if a
                    // future Phase A change ever splits routing (mosaic Bayer
                    // with a missing photosite, partial alignment failure that
                    // contributes to only some channels) the counts could
                    // diverge and the merge weights would silently miscount.
                    // Picking min keeps the weighted-mean conservative under
                    // any future divergence.
                    const auto& vox = cube.at(x, y);
                    const int64_t n_samples = static_cast<int64_t>(std::min({
                        vox.welford[ri].count(),
                        vox.welford[gi].count(),
                        vox.welford[bi].count()
                    }));

                    // NOTE: this block is single-threaded today. If anyone
                    // later parallelises the (y, x) loop (e.g. OpenMP over y),
                    // the ++derived.negative_clamped_count below becomes a
                    // data race; switch to std::atomic<int64_t> or a per-thread
                    // accumulator with reduction at that point.
                    for (int j = 0; j < lines.size(); ++j) {
                        double v = lines(j);
                        if (v < 0.0) {
                            v = 0.0;
                            ++derived.negative_clamped_count;
                        }
                        ensure_acc(fp.lines[j].name);
                        line_sum[fp.lines[j].name][p] += v * static_cast<double>(n_samples);
                        line_n  [fp.lines[j].name][p] += n_samples;
                    }
                }
            }
        }

        // Finalise the line slots: weighted mean.
        for (auto& [name, sumv] : line_sum) {
            auto& dst = derived.slots[name];
            dst.assign(N, 0.0f);
            for (int p = 0; p < N; ++p) {
                if (line_n[name][p] > 0) {
                    dst[p] = static_cast<float>(sumv[p] / static_cast<double>(line_n[name][p]));
                }
            }
        }

        result.derived = std::move(derived);
    }

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
