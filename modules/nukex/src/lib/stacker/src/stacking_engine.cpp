#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/stacker/cache_sig.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/coverage_trim.hpp"
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
#include "nukex/io/filter_alias.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/alignment/reference_selector.hpp"
#include "nukex/alignment/channel_registration.hpp"
// TASK-14-COLLAPSE: same — pImpl-only include, can move to the
// header once the namespace collision is gone.
#include "nukex/calibration/qe_database.hpp"
// TASK-14-COLLAPSE: same — for ChannelDecomposer pImpl.
#include "nukex/calibration/channel_decomposer.hpp"
#include "nukex/calibration/background_neutralization.hpp"
#include "nukex/calibration/frame_normalization.hpp"
#include <Eigen/Dense>
#include "nukex/classify/weight_computer.hpp"
// ColorComposer is module-owned (Task 11 / Task 12). The engine produces
// structured DerivedStack output; the module composes it for display.
#include "nukex/fitting/model_selector.hpp"
#include "nukex/fitting/huber_estimator.hpp"
#include "nukex/fitting/robust_stats.hpp"
#include "nukex/calibration/background_gradient.hpp"
#include "nukex/stacker/cache_paths.hpp"
#include "nukex/core/fits_time.hpp"
#include "nukex/combine/output_assembler.hpp"
#include "nukex/gpu/gpu_executor.hpp"
#include "nukex/gpu/gpu_context.hpp"

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
    // Empty path => the compiled-in database. A non-empty path is a
    // downloaded update or a test fixture loaded from disk. See Config docs.
    auto r1 = config_.qe_database_path.empty()
                  ? qe_database_->load_embedded()
                  : qe_database_->load_shipped(config_.qe_database_path);
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
        int s = static_cast<int>(voxel.channel(ch).distribution.shape);
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
    // get spurious error toasts. (The shipped DB is compiled in and
    // always loads; this ordering just avoids emitting any deferred parse
    // error before the user has actually asked for work.)
    ExecuteResult result;
    if (light_paths.empty()) return result;

    // Names the user has taught NukeX. A missing file is the ordinary first
    // run; a malformed one is reported and then ignored, so a hand-edited file
    // cannot stop a stack.
    if (!config_.filter_alias_path.empty()) {
        FilterAliasStore aliases;
        if (aliases.load(config_.filter_alias_path)) {
            filter_classifier_->set_aliases(std::move(aliases));
        } else {
            obs.message("Could not read the filter-alias file at "
                        + config_.filter_alias_path
                        + "; continuing with the shipped filter names only.");
        }
    }

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
        err.unknown_filter = first_filter.name;
        err.error = "FILTER='" + first_filter.name + "' on Bayer frame not in QE DB. "
                    "Rename FILTER to a known spelling, or add the filter to a "
                    "qe_overrides.json file and select it with the QE override picker "
                    "(see docs/qe_overrides_format.md). Remove FILTER to stack as plain OSC.";
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
    int n_ch = ch_config.n_channels;   // finalised by the measurement pass below

    const int n_frames = static_cast<int>(light_paths.size());

    // Build master flat if flats provided
    Image master_flat;
    if (!flat_paths.empty()) {
        master_flat = FlatCalibration::build_master_flat(flat_paths);
    }

    // Load a frame and put it through the same calibration the Phase A loop
    // applies, so measurements taken here describe the pixels that will
    // actually be aligned.
    auto load_calibrated = [&](int f, Image& out) -> bool {
        auto rr = FITSReader::read(light_paths[f]);
        if (!rr.success) return false;
        out = std::move(rr.image);
        // This frame's own CFA pattern, not frame 0's (see the Phase A loop).
        const BayerPattern fb = parse_bayer_pattern(rr.metadata.bayer_pattern);
        if (fb != BayerPattern::NONE) {
            out = DebayerEngine::debayer(out, fb);
        }
        if (!master_flat.empty()) {
            FlatCalibration::apply(out, master_flat);
        }
        return true;
    };

    // ═══ Measurement pass ════════════════════════════════════════════
    //
    // One read of every frame before the cube exists, answering two
    // questions that Phase A cannot answer while it streams.
    //
    // 1. Which slots does this batch need? ChannelConfig::merge() unions the
    //    slot names, so a batch whose later frames carry filters the first
    //    frame did not needs MORE channels than the first frame implies. The
    //    cube used to be allocated from the first frame alone and the config
    //    grown underneath it; that was survivable only because every voxel
    //    carried MAX_CHANNELS worth of per-channel arrays and the extra slots
    //    landed in space nobody was using. Now that a voxel is sized to its
    //    real channel count, the same growth writes off the end of the
    //    allocation. A mono L frame followed by a Bayer HaO3 frame is enough
    //    to do it: one channel allocated, four routed. So the union is
    //    settled here, before a single byte is allocated.
    //
    // 2. Which frame should be the alignment reference? It used to be
    //    whichever frame the directory listing put first. On an LRGB-mono set
    //    the per-filter star yield varies enormously, so that was a coin
    //    flip: on M27 2025 it landed on a blue frame with 32 stars, the Groth
    //    matcher could not find a consistent triangle against the 200-star
    //    frames that followed, and 71 of 72 frames aligned with zero inliers.
    //    Those frames are not merely dropped -- they are stacked unwarped at
    //    half weight, so a bad reference corrupts the result rather than just
    //    shrinking it.
    //
    // Cost is one extra read plus one star detection per frame, a few percent
    // of Phase A on these corpora.
    int ref_index = 0;
    std::vector<FrameQuality> frame_quality(n_frames);

    // Each frame's sky, one entry per image channel, tagged with the cube
    // slot that channel feeds. Keyed by NAME rather than index because the
    // slot union is not final until this pass completes. A frame the pass
    // rejects contributes nothing here and later gets the identity, which is
    // the honest answer for an exposure we could not measure.
    std::vector<std::vector<std::pair<std::string, FrameSky>>>
        frame_sky(n_frames);
    bool saw_bayer_frame = false;
    bool saw_mono_frame  = false;
    {
        obs.begin_phase("Measuring frames", n_frames);
        for (int f = 0; f < n_frames; f++) {
            auto rr = FITSReader::read(light_paths[f]);
            if (!rr.success) {
                frame_quality[f].usable = false;
                obs.advance(1, "  frame " + std::to_string(f + 1) + ": read failed");
                continue;
            }

            // Mirror the main loop's per-frame accept/reject exactly, so the
            // slot union here is the one Phase A will actually route into.
            Filter ff = filter_classifier_->classify(rr.metadata);
            const BayerPattern f_bayer = parse_bayer_pattern(rr.metadata.bayer_pattern);
            bool f_is_bayer = f_bayer != BayerPattern::NONE;
            if (ff.cls == FilterClass::UNKNOWN && f_is_bayer) {
                frame_quality[f].usable = false;
                if (result.unknown_filter.empty()) result.unknown_filter = ff.name;
                obs.advance(1, "  frame " + std::to_string(f + 1)
                               + ": unknown FILTER on Bayer -- will be skipped");
                continue;
            }
            ch_config = ChannelConfig::merge(ch_config, ChannelConfig::from_filter(ff));
            (f_is_bayer ? saw_bayer_frame : saw_mono_frame) = true;

            Image img = std::move(rr.image);
            if (f_bayer != BayerPattern::NONE) {
                img = DebayerEngine::debayer(img, f_bayer);
            }
            if (!master_flat.empty()) {
                FlatCalibration::apply(img, master_flat);
            }

            float sat = StarDetector::saturation_fraction(
                img, config_.aligner_config.star_config.saturation_level);
            if (sat >= config_.aligner_config.star_config.saturation_reject_fraction) {
                frame_quality[f].usable = false;
                obs.advance(1, "  frame " + std::to_string(f + 1) + ": blown out");
                continue;
            }

            // Sky, measured on exactly the image Phase A will accumulate:
            // debayered and flat-corrected, before any warp mixes in the
            // zeros the alignment leaves outside the source.
            {
                const ChannelConfig fcfg = ChannelConfig::from_filter(ff);
                // from_filter may declare more slots than the frame has
                // planes -- BROADBAND_OSC names R,G,B and a synthesized L for
                // a 3-plane image. Only the planes that exist are measured;
                // the synthesized slot inherits its inputs' correction.
                const int nmeas = std::min<int>(img.n_channels(), fcfg.n_channels);
                for (int c = 0; c < nmeas; ++c) {
                    if (fcfg.channel_names[c].empty()) continue;
                    frame_sky[f].emplace_back(
                        fcfg.channel_names[c],
                        measure_channel_sky(img.channel_data(c),
                                            img.width(), img.height()));
                }
            }

            StarCatalog cat =
                StarDetector::detect(img, config_.aligner_config.star_config);
            frame_quality[f].star_count  = static_cast<int>(cat.stars.size());
            frame_quality[f].median_fwhm = compute_median_fwhm(cat);

            char detail[96];
            std::snprintf(detail, sizeof(detail),
                          "  frame %d: %d stars, FWHM %.2f px",
                          f + 1, frame_quality[f].star_count,
                          frame_quality[f].median_fwhm);
            obs.advance(1, detail);

            if (obs.is_cancelled()) {
                obs.message("Cancelled while measuring frames.");
                obs.end_phase();
                return result;
            }
        }
        obs.end_phase();

        int chosen = select_reference_frame(frame_quality);
        ref_index = (chosen >= 0) ? chosen : 0;
    }

    // ─── Solve per-frame normalisation, one group per cube slot ──────────
    //
    // Per SLOT, not per batch. Measured on the user's own M27 2025 LRGB set
    // the per-filter sky runs L 2934, R 3447, G 4050, B 5222 ADU -- a single
    // batch-wide reference would scale L up 1.28x and B down 0.72x and
    // flatten the colour. Within one filter the spread is real and is what
    // this exists to remove: that set's B frames run 4383 to 12962 ADU.
    //
    // A frame the measuring pass rejected has no entry and keeps the
    // identity. So does a slot whose frames all measured a zero scale.
    std::vector<std::map<std::string, NormalizationCoefficients>>
        frame_norm(n_frames);
    if (config_.normalize_frames) {
        // slot name -> the frames that feed it, in frame order.
        std::map<std::string, std::vector<int>> group;
        for (int f = 0; f < n_frames; ++f)
            for (const auto& [name, sky] : frame_sky[f])
                group[name].push_back(f);

        int n_corrected = 0, n_total = 0;
        for (const auto& [name, members] : group) {
            n_total += static_cast<int>(members.size());
            std::vector<FrameSky> skies;
            skies.reserve(members.size());
            for (int f : members)
                for (const auto& [n2, sky] : frame_sky[f])
                    if (n2 == name) { skies.push_back(sky); break; }

            auto coeffs = solve_frame_normalization(skies);
            if (!config_.normalize_scale) {
                // Re-level only: keep each frame's own amplitude and move it
                // onto the common sky level. v' = v + (ref_loc - loc_f),
                // which is what the solved map degenerates to at scale 1.
                for (std::size_t i = 0; i < coeffs.size(); ++i) {
                    if (coeffs[i].scale == 1.0 && coeffs[i].offset == 0.0) continue;
                    const double mapped_loc =
                        coeffs[i].scale * skies[i].location + coeffs[i].offset;
                    coeffs[i].scale  = 1.0;
                    coeffs[i].offset = mapped_loc - skies[i].location;
                }
            }
            for (std::size_t i = 0; i < members.size(); ++i) {
                const auto& k = coeffs[i];
                frame_norm[members[i]][name] = k;
                // "Corrected" means the map actually moves a pixel. The
                // identity is exact, so an equality test is the right one.
                if (k.scale != 1.0 || k.offset != 0.0) n_corrected++;
            }
        }

        if (n_corrected > 0) {
            char msg[192];
            std::snprintf(msg, sizeof(msg),
                          "Sky normalisation: %d of %d frame-channels brought "
                          "onto their slot's reference level",
                          n_corrected, n_total);
            obs.message(msg);
        } else {
            obs.message("Sky normalisation: session is stable, nothing to correct");
        }
    }

    // The slot union is now final; everything downstream sizes against it.
    n_ch = ch_config.n_channels;

    // Guard: more than one slot fed by un-debayered (single-channel) frames.
    //
    // FrameCache is keyed on post-debayer geometry, so every mono frame in a
    // batch lands in the same (W, H, 1) cache regardless of its filter. Phase B
    // then has no way to read a given slot's own frames: one slot reads that
    // cache and fits a mixture of every filter's samples, and the rest are
    // routed to no cache at all and fit a buffer of zeros. On M27 2025
    // (L24 R12 G12 B24) that produced one populated channel out of four and
    // three exactly-zero ones -- an image that looked like a colour-balance
    // problem and was not.
    //
    // Phase A routes correctly; the per-slot Welford accumulators are right.
    // The gap is that Phase B's read path, shadow buffers and weight kernels
    // all assume every channel shares one frame set. Separating them is an
    // architectural change, not a cache key, so until it lands this refuses to
    // run rather than emitting channels that are silently empty.
    // Guard: a batch that mixes Bayer and mono frames.
    //
    // The batch-level Bayer pattern comes from frame 0 alone and the Phase A
    // loop debayers on that one global, so the two orderings are wrong in
    // different directions. Mono first: the Bayer frame is never demosaiced
    // and the OSC routing branches read channels 1 and 2 of a one-channel
    // image, which Image::at does not bounds-check. Bayer first: every mono
    // frame IS demosaiced as though it were a CFA mosaic, and BROADBAND_L
    // routes channel 0 of that fabricated image into the L slot -- no fault,
    // wrong pixels.
    //
    // Debayering per frame would fix the read, but it would then put two
    // geometries in one batch, which lands on the same Phase B limitation as
    // multi-filter mono: FrameCache is keyed on geometry and cannot keep two
    // frame sets apart. So refuse until that is addressed.
    if (saw_bayer_frame && saw_mono_frame) {
        ExecuteResult err{};
        err.ok    = false;
        err.error = "This batch mixes Bayer (CFA) frames with mono frames. "
                    "NukeX debayers a batch according to its first frame, so "
                    "one of the two groups would be decoded the wrong way -- "
                    "silently, in the direction that does not crash. Stack the "
                    "Bayer frames and the mono frames separately and combine "
                    "the results.";
        obs.message(err.error);
        return err;
    }

    if (path_is_ram_backed(config_.cache_dir)) {
        obs.message("WARNING: cache directory " + config_.cache_dir +
                    " is on a RAM-backed filesystem (tmpfs). The frame cache and "
                    "the voxel record will live in memory, which is exactly what "
                    "they exist to avoid -- a 33-frame 24 MP session writes 13 GB "
                    "of cache. Choose a directory on a disk in the NukeX interface.");
    }

    // Allocate cube -- file-backed in the cache directory when it would
    // crowd memory, in memory otherwise (the file costs ~30% wall time on a
    // 24 MP stack, measured; swapping costs far more).
    Cube cube;
    std::string cube_backing;
    {
        const std::size_t need = voxel_record_size(ch_config.n_channels)
                               * static_cast<std::size_t>(out_width)
                               * static_cast<std::size_t>(out_height);
        const std::size_t avail = GPUContext::host_available_bytes();
        const bool crowds = avail > 0 &&
            static_cast<double>(need) > config_.file_backed_cube_fraction * static_cast<double>(avail);
        if (config_.file_backed_cube && crowds) {
            try {
                cube = Cube(out_width, out_height, ch_config, config_.cache_dir);
                char why[160];
                std::snprintf(why, sizeof(why),
                              "file-backed in %s (%.0f%% of the %.1f GB available)",
                              config_.cache_dir.c_str(),
                              100.0 * static_cast<double>(need) / static_cast<double>(avail),
                              static_cast<double>(avail) / 1e9);
                cube_backing = why;
            } catch (const std::exception& e) {
                obs.message(std::string("Voxel record: file backing unavailable (") + e.what()
                            + "); holding it in memory instead.");
            }
        }
        if (!cube.file_backed()) {
            cube = Cube(out_width, out_height, ch_config);
            char why[160];
            if (avail > 0)
                std::snprintf(why, sizeof(why), "in memory (%.0f%% of the %.1f GB available)",
                              100.0 * static_cast<double>(need) / static_cast<double>(avail),
                              static_cast<double>(avail) / 1e9);
            else
                std::snprintf(why, sizeof(why), "in memory");
            cube_backing = why;
        }
    }
    {
        // The single number that decides whether this run fits in RAM. Each
        // voxel is sized to the stack's real channel count, so this scales
        // with n_channels rather than the MAX_CHANNELS ceiling. File-backed,
        // its pages are page cache the kernel can drop and re-read; in
        // memory, they are what the machine has to swap.
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "Voxel record: %.2f GB (%dx%d px x %d channels, %zu B/voxel), %s",
                      static_cast<double>(cube.bytes_allocated()) / 1e9,
                      out_width, out_height, n_ch, cube.voxel_stride(),
                      cube_backing.c_str());
        obs.message(msg);
    }

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
    std::map<CacheSig, FrameCache> caches;
    auto get_or_create_cache = [&](int w, int h, int n_ch,
                                   const std::string& routing_key) -> FrameCache& {
        CacheSig sig{w, h, n_ch, routing_key};
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
    std::set<std::string> unknown_instrume_warned;

    // Frame-level metadata
    std::vector<FrameStats> frame_stats(n_frames);
    std::vector<float> frame_fwhms(n_frames, 0.0f);

    // Initialize aligner, seeded with the reference the measurement pass chose.
    FrameAligner aligner(config_.aligner_config);
    if (n_frames > 1) {
        Image ref_image;
        if (load_calibrated(ref_index, ref_image)) {
            aligner.set_reference(ref_image, ref_index);
            std::string ref_name = light_paths[ref_index];
            auto slash = ref_name.rfind('/');
            if (slash != std::string::npos) ref_name = ref_name.substr(slash + 1);
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "Alignment reference: frame %d/%d (%s) -- %d stars, FWHM %.2f px",
                          ref_index + 1, n_frames, ref_name.c_str(),
                          frame_quality[ref_index].star_count,
                          frame_quality[ref_index].median_fwhm);
            obs.message(msg);
        } else {
            // Could not re-read the frame we picked. Leave the aligner without
            // a reference so it falls back to the first frame to arrive, and
            // say so rather than failing silently.
            obs.message("Could not re-read the chosen alignment reference; "
                        "falling back to the first frame.");
        }
    }

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
                                float value,
                                bool covered) {
        // An uncovered pixel is not a measurement of darkness, it is the
        // absence of a measurement. warp leaves 0 outside the source and 0
        // is a legal pixel value, so without this guard every frame's
        // dither excursion was averaged in as black: measured on a 53-frame
        // M3 stack, a rim at 48.6% of interior brightness reaching 48 px
        // deep, and 268,802 exactly-zero pixels.
        if (!covered) return;
        voxel.channel(idx).welford.update(value);
        if (voxel.channel(idx).welford.count() == 1) {
            voxel.channel(idx).histogram.initialize_range(value - 0.1f, value + 0.1f);
        }
        voxel.channel(idx).histogram.update(value);
    };

    // An empty mask means the frame was cloned rather than warped, so it
    // covers itself completely.
    auto covered_at = [](const CoverageMask& cov, int c, int y, int x) {
        return cov.empty() || cov.covered(c, y, x);
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
        //
        // frame_bayer is THIS frame's own CFA pattern. The outer `bayer` comes
        // from light_paths[0] alone; mixed Bayer/mono batches are refused
        // above, but an all-Bayer batch can still mix patterns (two cameras,
        // or a header that flips the CFA with the image), and debayering
        // every frame with frame 0's pattern would swap R and B in the rest.
        BayerPattern frame_bayer = parse_bayer_pattern(meta.bayer_pattern);
        bool frame_is_bayer = frame_bayer != BayerPattern::NONE;
        if (frame_filter.cls == FilterClass::UNKNOWN && frame_is_bayer) {
            obs.advance(1, "  skipped -- unknown FILTER='" + frame_filter.name +
                           "' on Bayer frame (add it to a qe_overrides.json selected in the NukeX interface to recover)");
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
        // Resolve the raw INSTRUME onto a DB key here so the guard compares
        // DB identities ("asi2400mc"), not header spellings.
        if (frame_filter.cls == FilterClass::DUAL_NB_OSC) {
            std::string key = qe_database_->resolve_camera(frame_filter.camera);
            if (key.empty()) {
                key = QEDatabase::kGenericOSCCamera;
                result.qe_generic_camera_fallback = true;
                if (unknown_instrume_warned.insert(frame_filter.camera).second) {
                    obs.message("Camera '" + frame_filter.camera +
                                "' unknown; using generic Sony IMX OSC QE values. "
                                "Output marked as low-confidence (NUKEX_QE_CONFIDENCE).");
                }
            }
            dual_nb_cameras.insert(key);
        }

        // Merge per-frame config into the cube's running channel_config.
        // The measurement pass already unioned every frame's slots, so this
        // is idempotent -- and it must be. A voxel holds exactly
        // allocated_channels() channel records, so a slot index beyond that
        // writes into the next voxel, and past the end of the last one it
        // corrupts the heap. Before the voxel was sized to its real channel
        // count the same growth was absorbed by MAX_CHANNELS of unused
        // provisioning and nobody noticed. Fail loudly instead.
        ChannelConfig per_frame_cfg = ChannelConfig::from_filter(frame_filter);
        ChannelConfig merged = ChannelConfig::merge(cube.channel_config, per_frame_cfg);
        if (merged.n_channels > cube.allocated_channels()) {
            // Reachable without a programmer error: if the measurement pass
            // could not read this frame but Phase A could -- a transient I/O
            // condition -- its filter never entered the union. std::abort()
            // here would take the whole PixInsight session down with no
            // chance to save, so fail the stack loudly instead.
            ExecuteResult err{};
            err.ok    = false;
            err.error = "Frame " + std::to_string(f + 1) + " needs channel slot "
                        + std::to_string(merged.n_channels)
                        + " but the cube was allocated for "
                        + std::to_string(cube.allocated_channels())
                        + ". Its filter was not seen when the frames were "
                        "measured, which usually means that frame could not be "
                        "read at that point. Check the file is readable and "
                        "run the stack again.";
            obs.message(err.error);
            obs.end_phase();
            return err;
        }
        cube.channel_config = merged;

        // 2. Debayer -- THIS frame's pattern (frame_bayer), not frame 0's.
        if (frame_bayer != BayerPattern::NONE) {
            obs.advance(0, "  debayering (" + meta.bayer_pattern + ")");
            image = DebayerEngine::debayer(image, frame_bayer);
        }

        // 3. Flat correct
        if (!master_flat.empty()) {
            obs.advance(0, "  flat correcting");
            FlatCalibration::apply(image, master_flat);
        }

        // 4. Align
        // Pre-check saturation so a blown-out frame gets a specific log line
        // ("SKIPPED -- blown out, X%") rather than the generic
        // "aligned: FAILED (stars=0)".  The actual guard that keeps
        // StarDetector fast lives in StarDetector::detect — this is purely
        // for log clarity in the Process Console.
        float sat_frac = StarDetector::saturation_fraction(
            image, config_.aligner_config.star_config.saturation_level);
        bool blown_out =
            sat_frac >= config_.aligner_config.star_config.saturation_reject_fraction;

        obs.advance(0, "  aligning");
        auto aligned = aligner.align(image, f, parse_fits_datetime(meta.date_obs));
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
                std::string("  aligned: SKIPPED (blown out -- ") + pct
                + "% pixels at saturation)");
        } else {
           const auto& a = aligned.alignment;
           // A chained frame is aligned, but it reached the reference through
           // a neighbour rather than directly -- worth saying, because a run
           // full of them means the session drifted far enough that the
           // single-reference matcher could not bridge it.
           std::string status = a.alignment_failed
                              ? "FAILED"
                              : (a.chained
                                 ? ("ok (chained via frame "
                                    + std::to_string(a.chained_via + 1) + ")")
                                 : "ok");
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

        // Channel registration, when there is something to say about it.
        // AlignedFrame::channels is populated whenever a colour frame was
        // measured, whether or not the correction was applied, so silence
        // alone cannot be the guard here -- it would fire on every frame of a
        // well-corrected rig, each reporting a few negligible ppm that was
        // never applied. Report a frame only when the correction actually
        // moved pixels, or when the fit gave up on some non-reference
        // channel: that second case is a frame with too few isolated stars
        // to measure, which is the opposite situation from "nothing was
        // wrong" and must not look the same in the log.
        if (!aligned.channels.empty()) {
            const double corner_radius =
                std::hypot(image.width() / 2.0, image.height() / 2.0);
            const bool applied = !aligned.channels.negligible(
                corner_radius, kNegligibleChannelShiftPx);
            bool gave_up = false;
            for (size_t ch = 0; ch < aligned.channels.per_channel.size(); ch++) {
                if (static_cast<int>(ch) == aligned.channels.reference_channel)
                    continue;
                if (aligned.channels.per_channel[ch].fit ==
                    ChannelTransform::Fit::Identity) {
                    gave_up = true;
                    break;
                }
            }
            if (applied || gave_up) {
                const std::string desc =
                    describe_channel_transforms(aligned.channels, corner_radius);
                // No emptiness check: describe_channel_transforms always
                // returns text when a correction was applied or a channel
                // gave up, and those are exactly the two conditions above.
                obs.advance(0, "  channel reg: " + desc);
            }
        }

        // 4b. Frame median, taken BEFORE normalisation.
        //
        // It feeds cloud detection below, which compares this frame's level
        // to the median of all frames' levels. Normalisation exists precisely
        // to make those levels equal, so measuring afterwards would leave
        // every frame looking clear and silently retire the cloud penalty.
        // A cloudy frame is genuinely noisier even once rescaled, so it must
        // still be recognisable as one.
        float frame_median = compute_frame_median(aligned.image);

        // 4c. Bring this frame onto its slots' common level and spread.
        //
        // After alignment, so saturation rejection and star detection all
        // still see raw values and behave exactly as before; before caching,
        // so the cache and the cube agree. The warp leaves zeros outside the
        // source and the offset lifts those too, which is harmless: every
        // consumer gates on the coverage mask, never on the value.
        if (config_.normalize_frames && !frame_norm[f].empty()) {
            const std::size_t npx =
                static_cast<std::size_t>(aligned.image.width())
              * aligned.image.height();
            for (int c = 0; c < aligned.image.n_channels(); ++c) {
                const std::string& slot = per_frame_cfg.channel_names[c];
                if (slot.empty()) continue;
                auto it = frame_norm[f].find(slot);
                if (it == frame_norm[f].end()) continue;
                const float a = static_cast<float>(it->second.scale);
                const float b = static_cast<float>(it->second.offset);
                if (a == 1.0f && b == 0.0f) continue;   // exact identity
                float* d = aligned.image.channel_data(c);
                for (std::size_t i = 0; i < npx; ++i) d[i] = a * d[i] + b;
            }
        }

        // 5. Cache aligned frame into the geometry-matched cache.
        obs.advance(0, "  caching");
        // A single-channel frame is routed by its slot, so each mono filter
        // gets its own cache and Phase B can read only that filter's frames.
        // Multi-channel frames serve several slots by channel index from one
        // cache, which was always correct, so they share the empty key.
        const std::string routing_key =
            (aligned.image.n_channels() == 1 && !per_frame_cfg.channel_names[0].empty())
                ? per_frame_cfg.channel_names[0]
                : std::string();
        get_or_create_cache(aligned.image.width(),
                            aligned.image.height(),
                            aligned.image.n_channels(),
                            routing_key).write_frame(aligned.image, f, aligned.coverage);

        // 6. Frame-level stats
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

        // Record what was applied, indexed by cube slot, for Phase B's noise
        // model -- its Poisson term is only meaningful on raw ADU and it has
        // no other way to recover it.
        if (config_.normalize_frames) {
            for (const auto& [slot, k] : frame_norm[f]) {
                const int si = cube.channel_config.slot_index(slot);
                if (si < 0 || si >= MAX_CHANNELS) continue;
                frame_stats[f].norm_scale[si]  = static_cast<float>(k.scale);
                frame_stats[f].norm_offset[si] = static_cast<float>(k.offset);
            }
            // A synthesized slot was never normalised directly -- it was
            // built in the router below out of planes that already had been.
            // Give the noise model the map that was effectively applied to
            // it rather than the identity, which would understate the noise
            // of every rescaled frame.
            if (frame_filter.cls == FilterClass::BROADBAND_OSC) {
                const int li = cube.channel_config.slot_index("L");
                const int ri = cube.channel_config.slot_index("R");
                const int gi = cube.channel_config.slot_index("G");
                const int bi = cube.channel_config.slot_index("B");
                if (li >= 0 && ri >= 0 && gi >= 0 && bi >= 0) {
                    const NormalizationCoefficients in[3] = {
                        {frame_stats[f].norm_scale[ri], frame_stats[f].norm_offset[ri]},
                        {frame_stats[f].norm_scale[gi], frame_stats[f].norm_offset[gi]},
                        {frame_stats[f].norm_scale[bi], frame_stats[f].norm_offset[bi]},
                    };
                    // The same rec709 weights the router mixes L with.
                    const double w[3] = {0.299, 0.587, 0.114};
                    const auto m = mix_coefficients(in, w, 3);
                    frame_stats[f].norm_scale[li]  = static_cast<float>(m.scale);
                    frame_stats[f].norm_offset[li] = static_cast<float>(m.offset);
                }
            }
        }

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
                        const bool cr = covered_at(aligned.coverage, 0, y, x);
                        const bool cg = covered_at(aligned.coverage, 1, y, x);
                        const bool cb = covered_at(aligned.coverage, 2, y, x);
                        route_sample_idx(voxel, r_idx, r, cr);
                        route_sample_idx(voxel, g_idx, g, cg);
                        route_sample_idx(voxel, b_idx, b, cb);
                        // Synthetic luminance mixes all three planes, so it
                        // is only a measurement where all three are.
                        route_sample_idx(voxel, l_idx, l, cr && cg && cb);
                        if (cr || cg || cb) voxel.n_frames++;
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
                        const bool cr = covered_at(aligned.coverage, 0, y, x);
                        const bool cg = covered_at(aligned.coverage, 1, y, x);
                        const bool cb = covered_at(aligned.coverage, 2, y, x);
                        route_sample_idx(voxel, r_idx, aligned.image.at(x, y, 0), cr);
                        route_sample_idx(voxel, g_idx, aligned.image.at(x, y, 1), cg);
                        route_sample_idx(voxel, b_idx, aligned.image.at(x, y, 2), cb);
                        if (cr || cg || cb) voxel.n_frames++;
                    }
                }
                break;
            }
            case FilterClass::BROADBAND_L:
            case FilterClass::NARROWBAND_SINGLE:
            case FilterClass::BROADBAND_RGB: {
                // Mono frames: channel 0 of the (post-debayer) image. The
                // slot name comes from from_filter() — "L" for broadband-L,
                // "Ha"/"OIII"/"SII" for narrowband-single, "R"/"G"/"B"
                // for an explicit R/G/B mono filter.
                // Route by the slot merge() actually registered for this
                // frame's class — from_filter() maps BROADBAND_L to "L"
                // regardless of Filter.name (which may be "L_unnamed" or a
                // raw unknown FILTER value such as a wheel-slot number),
                // and R/G/B or Ha/OIII/SII to the name itself. Looking up
                // frame_filter.name directly aborted on every mono frame
                // whose FILTER was not literally "L".
                const std::string& slot_name = per_frame_cfg.channel_names[0];
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
                        const bool c0 = covered_at(aligned.coverage, 0, y, x);
                        route_sample_idx(voxel, slot_idx, aligned.image.at(x, y, 0), c0);
                        if (c0) voxel.n_frames++;
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

    // Phase A is done writing. Schedule the last writeback now, so the
    // dirty pages are not still queued behind Phase B's allocations.
    // write_frame() only syncs every 8th frame -- see the note there on the
    // 26x write amplification a per-frame sync costs.
    for (auto& [sig, cache] : caches) cache.flush();

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
    // Mono caches are now keyed by the slot they route into, so each filter
    // has its own and a slot can read only its own frames.
    std::map<std::string, const FrameCache*> mono_cache_by_slot;
    for (auto& [sig, c] : caches) {
        if (std::get<2>(sig) == 3 && cache_3ch == nullptr) cache_3ch = &c;
        if (std::get<2>(sig) == 1) mono_cache_by_slot.emplace(std::get<3>(sig), &c);
    }
    auto mono_cache_for = [&](const std::string& slot) -> const FrameCache* {
        auto it = mono_cache_by_slot.find(slot);
        return it == mono_cache_by_slot.end() ? nullptr : it->second;
    };

    for (int slot_i = 0; slot_i < n_slots; ++slot_i) {
        const std::string& name = cube.channel_config.slot_name(slot_i);

        if (name == "L") {
            // Two cases: raw mono-L filter (1ch cache) or BROADBAND_OSC
            // synthesised L (3ch cache, no raw L in cache).
            if (const FrameCache* mono_L = mono_cache_for("L")) {
                // Raw L filter: direct read from channel 0 of L's own cache.
                slot_cache_refs[slot_i] = {mono_L, 0, SlotSynthesis::DIRECT};
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
            } else if (const FrameCache* mono = mono_cache_for(name)) {
                // Mono R, G or B filter. Each has its own cache now, keyed on
                // the slot it routes into, so three mono colour filters no
                // longer collapse into one file and read each other's frames.
                slot_cache_refs[slot_i] = {mono, 0, SlotSynthesis::DIRECT};
            }

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
            // Each comes from its own mono cache, channel 0.
            if (const FrameCache* mono = mono_cache_for(name)) {
                slot_cache_refs[slot_i] = {mono, 0, SlotSynthesis::DIRECT};
            }
        }
    }

    // ═══ PHASE B — Analysis (GPU-accelerated) ════════════════════════

    ModelSelector fitter(config_.fitting_config);
    HuberEstimator huber;
    const bool use_huber = (config_.estimator == Config::Estimator::HUBER);
    obs.message(use_huber
        ? "Estimator: Huber M-estimator (median seed, MAD scale, tuning 1.345)."
        : "Estimator: distribution model race (Student-t / GMM / Contamination / KDE by AICc).");

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
    auto fitting_fn = [&fitter, &huber, use_huber](SubcubeVoxel& voxel,
                                 const float* values, const float* weights,
                                 int stride, int nc,
                                 const FrameStats* /*fs*/,
                                 const int* nf_ch) {
        for (int ch = 0; ch < nc; ch++) {
            // stride addresses the row; nf_ch[ch] says how much of it is
            // real. Fitting the whole row would average a short channel's
            // samples against zero padding -- on an L24/R12 batch that
            // halved R.
            const int n = nf_ch ? nf_ch[ch] : stride;
            if (n <= 0) continue;
            if (use_huber)
                huber.estimate(values + ch * stride, weights + ch * stride, n, voxel, ch);
            else
                fitter.select(values + ch * stride, weights + ch * stride, n, voxel, ch);
        }
    };

    // The WIDEST frame set in the batch. Caches are no longer written in
    // lockstep -- each mono filter has its own and holds only its own frames
    // -- so this is the buffer stride, not a count. Each channel's real count
    // comes from its own cache via ShadowBuffers::n_frames. Taking the first
    // cache's count instead would size the buffers to whichever filter sorted
    // first and silently truncate every longer one.
    int n_frames_written = 0;
    for (const auto& [sig, c] : caches)
        n_frames_written = std::max(n_frames_written, c.n_frames_written());

    // Half-stacks for the noise decomposition, from the same samples and the
    // same estimator as the stack itself.
    Image half_even(out_width, out_height, n_ch), half_odd(out_width, out_height, n_ch);
    GPUExecutor::HalfStackFn half_fn = [&fitter, &huber, use_huber](const float* v, const float* w, int n) -> float {
        if (n <= 0) return 0.0f;
        if (use_huber) {
            std::vector<float> scratch(static_cast<std::size_t>(n));
            return HuberEstimator::location(v, w, n, HuberEstimator::Config{}, scratch.data());
        }
        return fitter.select_best(v, w, n).distribution.true_signal_estimate;
    };

    auto phase_b_start = std::chrono::steady_clock::now();
    gpu.execute_phase_b(cube, slot_cache_refs, n_frames_written,
                        frame_stats, config_.weight_config,
                        fitting_fn, stacked, noise_map, &obs,
                        half_fn, &half_even, &half_odd);
    auto phase_b_end = std::chrono::steady_clock::now();
    long phase_b_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          phase_b_end - phase_b_start ).count();
    obs.message("Phase B: " + std::to_string(phase_b_ms) + " ms");

    if (obs.is_cancelled()) {
        obs.message("Cancelled during distribution fitting.");
        return result;
    }

    // ═══ Sky gradient ════════════════════════════════════════════════
    //
    // Applied before Phase C so the spatial context, the measured-noise map
    // and the Q-solved derived slots all see the flattened sky. Only the tilt
    // comes off; the sky LEVEL is left alone because the auto-stretch reads it
    // as its shadow target.
    //
    // The plane is SAMPLED inside the rectangle every frame contributed to.
    // The thin-coverage rim outside it is several times noisier than the
    // interior and sits at maximum leverage for a plane fit, so sampling it
    // manufactures a tilt. The same rectangle is applied as the coverage trim
    // below; it is computed once here.
    const TrimBounds coverage = full_coverage_rect(cube);
    if (config_.remove_sky_gradient && !stacked.empty()) {
        const FitRegion region{coverage.x0, coverage.y0, coverage.x1, coverage.y1};
        const FitRegion* roi = coverage.applied ? &region : nullptr;
        const int nch = stacked.n_channels();
        std::vector<GradientModel> models(nch);

        // A synthesized L slot (OSC: 0.299 R + 0.587 G + 0.114 B per frame) is
        // not fitted on its own. Its tilt IS that combination of the R, G and
        // B tilts, and fitting it separately -- with its own seed and its own
        // clipped sample set -- removed a slightly different plane, so the
        // luminance the composer reads no longer matched the chroma planes
        // beside it. Physical slots are fitted first; the synthesized one
        // takes the combination.
        auto is_synth_L = [&](int ch) {
            return ch < static_cast<int>(slot_cache_refs.size())
                && slot_cache_refs[ch].kind == SlotSynthesis::REC709_LUMA
                && cube.channel_config.slot_name(ch) == "L";
        };
        for (int ch = 0; ch < nch; ch++) {
            if (is_synth_L(ch)) continue;
            models[ch] = BackgroundGradient::fit_planar(stacked, ch, roi);
        }
        for (int ch = 0; ch < nch; ch++) {
            if (!is_synth_L(ch)) continue;
            const int r = cube.channel_config.slot_index("R");
            const int g = cube.channel_config.slot_index("G");
            const int b = cube.channel_config.slot_index("B");
            if (r >= 0 && g >= 0 && b >= 0 && r < nch && g < nch && b < nch &&
                models[r].valid && models[g].valid && models[b].valid) {
                GradientModel& m = models[ch];
                m.dx = 0.299 * models[r].dx + 0.587 * models[g].dx + 0.114 * models[b].dx;
                m.dy = 0.299 * models[r].dy + 0.587 * models[g].dy + 0.114 * models[b].dy;
                m.level = 0.299 * models[r].level + 0.587 * models[g].level + 0.114 * models[b].level;
                m.valid = true;
            }
        }
        for (int ch = 0; ch < nch; ch++) {
            const GradientModel& m = models[ch];
            if (!m.valid) {
                obs.message("Sky gradient: channel " + std::to_string(ch)
                            + " -- too little background to fit, left as is.");
                continue;
            }
            const double amp = BackgroundGradient::amplitude(m);
            BackgroundGradient::subtract(stacked, ch, m);
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "Sky gradient: channel %d%s -- removed a tilt of %.3e "
                          "corner to corner (dx %+.3e, dy %+.3e)",
                          ch, is_synth_L(ch) ? " (synthesized L: rec709 of the R, G, B tilts)" : "",
                          amp, m.dx, m.dy);
            obs.message(msg);
        }
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
            for (int ch = 0; ch < n_ch; ch++) avg_snr += voxel.channel(ch).snr;
            avg_snr /= n_ch;
            float cloud_fraction = (voxel.n_frames > 0) ?
                static_cast<float>(voxel.cloud_frame_count) / voxel.n_frames : 0.0f;
            voxel.quality_score = voxel.channel(0).distribution.confidence
                * (1.0f - cloud_fraction)
                * std::min(1.0f, avg_snr / 50.0f);
            voxel.confidence = voxel.channel(0).distribution.confidence;
        }
    }

    // Spatial context (GPU kernel 4)
    obs.advance(0, "spatial context");
    // The luminance the spatial kernel measures noise on, by slot NAME: the
    // L plane when there is one, else R, G, B by name, else positional.
    const LuminanceSpec noise_luminance =
        LuminanceSpec::for_config(cube.channel_config, stacked.n_channels());
    gpu.execute_spatial_context(stacked, cube, &obs, noise_luminance);
    obs.advance(1);

    obs.end_phase();

    // Quality map
    Image quality_map = OutputAssembler::assemble_quality_map(cube);

    // Measured noise: the stack's realised pixel-to-pixel scatter, beside the
    // predicted noise_map. They answer different questions, and until now only
    // the prediction existed -- which is why an estimator that adds noise of
    // its own was invisible to NukeX's own diagnostics.
    Image measured_noise = OutputAssembler::assemble_measured_noise(cube);

    // ═══ PHASE B FOLLOW-UP — Q-solve derived semantic slots ═══════════
    //
    // Kernel 3 (pixel selection) wrote the per-pixel best raw value for each cube slot
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
                        vox.channel(ri).welford.count(),
                        vox.channel(gi).welford.count(),
                        vox.channel(bi).welford.count()
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

    // Intersection: keep only the rectangle every frame contributed to.
    //
    // A dithered, drifting session does not cover a rectangle. The outer ring
    // is reached by fewer and fewer frames, and those pixels are averaged over
    // less data -- correctly, but it shows. Measured on a 74-frame stack
    // against an interior noise of 0.000234: the top ten rows carry 0.001093
    // (4.7x) and the right ten columns 0.000837 (3.6x). At the contrast the
    // shadow point now delivers, those bands read as a frame around the
    // picture. The one genuinely dead line -- a red column pushed off the
    // source by channel registration -- goes with them.
    {
        const TrimBounds& trim = coverage;
        if (trim.applied) {
            const int tw = trim.width(), th = trim.height();
            Image ts = stacked.cropped(trim.x0, trim.y0, tw, th);
            Image tn = noise_map.cropped(trim.x0, trim.y0, tw, th);
            Image tq = quality_map.cropped(trim.x0, trim.y0, tw, th);
            Image tm = measured_noise.cropped(trim.x0, trim.y0, tw, th);
            // A refused crop returns an empty image; keeping the untrimmed
            // frame is the only safe answer, and silence is not.
            if (ts.empty() || tn.empty() || tq.empty() || tm.empty()) {
                obs.message("Coverage trim skipped: the computed rectangle did "
                            "not fit the output.");
            } else {
                stacked        = std::move(ts);
                noise_map      = std::move(tn);
                quality_map    = std::move(tq);
                measured_noise = std::move(tm);
                for (auto& [name, plane] : result.derived.slots) {
                    if (plane.size() != static_cast<std::size_t>(out_width) *
                                        static_cast<std::size_t>(out_height))
                        continue;
                    std::vector<float> cut(static_cast<std::size_t>(tw) * th);
                    for (int row = 0; row < th; ++row)
                        std::copy(plane.begin() + static_cast<std::size_t>(trim.y0 + row) * out_width + trim.x0,
                                  plane.begin() + static_cast<std::size_t>(trim.y0 + row) * out_width + trim.x0 + tw,
                                  cut.begin() + static_cast<std::size_t>(row) * tw);
                    plane.swap(cut);
                }
                if (!result.derived.slots.empty()) {
                    result.derived.width  = tw;
                    result.derived.height = th;
                }
                result.trim = trim;
                char buf[192];
                std::snprintf(buf, sizeof(buf),
                              "Coverage trim: %dx%d -> %dx%d (kept the region every "
                              "frame contributed to; origin %d,%d).",
                              out_width, out_height, tw, th, trim.x0, trim.y0);
                obs.message(buf);
            }
        }
    }

    // Match the chroma slots' sky levels before anything downstream sees them.
    //
    // A broadband stack's green cast is an ADDITIVE sky pedestal -- filter and
    // light pollution, weighted by QE -- not a scaling error. Measured on a
    // 74-frame OSC stack the background sits at G/R = 1.54 while the signal,
    // once each channel's own background is removed, sits at 1.07. Every
    // stretch preserves colour ratios, so that 1.54 rides all the way to the
    // finished image: the user's M63 JPEG measured G/R = 1.52 after GHS,
    // curves and a crop. A pedestal comes off by subtraction, in linear
    // space, or not at all -- and it has to come off HERE, because the linear
    // stack is what leaves NukeX for the rest of a user's workflow.
    {
        // The derived slots are matched HERE, beside the stacked image and
        // after the trim, not where they were built. Both windows have to
        // measure their background over the same pixels: matching derived
        // before the trim would have measured it across the thin, noisy rim
        // the trim then removed, and the composed window would have disagreed
        // with the stretched one by exactly that difference.
        {
            const std::size_t n = static_cast<std::size_t>(stacked.width())
                                * static_cast<std::size_t>(stacked.height());
            std::vector<float*> chroma;
            for (const char* nm : {"R", "G", "B"}) {
                auto it = result.derived.slots.find(nm);
                if (it != result.derived.slots.end() && it->second.size() == n)
                    chroma.push_back(it->second.data());
            }
            match_plane_backgrounds(chroma, n);
        }

        const BackgroundOffsets bg = neutralize_chroma_slots(stacked, cube.channel_config);
        if (bg.applied) {
            std::string msg = "Background matched to the dimmest colour channel; subtracted";
            const std::vector<int> idx = chroma_slot_indices(cube.channel_config);
            for (std::size_t k = 0; k < idx.size() && k < bg.subtracted.size(); ++k) {
                const std::string& name = cube.channel_config.slot_name(idx[k]);
                result.background_match.emplace_back(name, bg.subtracted[k]);
                char buf[64];
                std::snprintf(buf, sizeof(buf), " %s %.5f", name.c_str(),
                              static_cast<double>(bg.subtracted[k]));
                msg += buf;
            }
            obs.message(msg + ".");
        }
    }

    // The one number that makes estimator-injected noise visible. NukeX's
    // noise_map is a model of what the noise ought to be; measured_noise is
    // what it is. A ratio above 1 says the stack is noisier than its own model
    // predicts, which is exactly the signature that hid for three releases.
    // Both medians are printed with the ratio, and the ratio IS their
    // quotient, so a reader who divides gets the number on the line.
    {
        const OutputAssembler::NoiseCheck nc =
            OutputAssembler::noise_check(measured_noise, noise_map, noise_luminance);
        if (nc.comparable) {
            // Which basis the prediction stands on. The CCD model needs gain
            // AND read noise from the FITS header; without them a frame's
            // predicted variance is the robust across-frame scale, and a
            // reader should know which one they are looking at, because the
            // two can disagree by the very factor the check is meant to show.
            int with_keywords = 0;
            for (const auto& fs : frame_stats)
                if (fs.has_noise_keywords) ++with_keywords;
            char msg[320];
            std::snprintf(msg, sizeof(msg),
                          "Noise model: %d of %zu frames carry usable gain and "
                          "read-noise keywords (CCD model); the rest use the "
                          "robust across-frame scale.",
                          with_keywords, frame_stats.size());
            obs.message(msg);
            std::snprintf(msg, sizeof(msg),
                          "Noise check on %s: measured %.3e, predicted %.3e, "
                          "ratio %.2fx (1.00x means the stack is as clean as "
                          "its model says)",
                          noise_luminance.describe(&cube.channel_config).c_str(),
                          nc.measured_median, nc.predicted_median, nc.ratio);
            obs.message(msg);

            // What the ratio is made of. Half the samples against the other
            // half: what cancels was in every frame and is not noise the
            // estimator made -- it is what flats remove.
            const OutputAssembler::NoiseDecomposition nd =
                OutputAssembler::noise_decomposition(nc.measured_median, half_even, half_odd, noise_luminance);
            if (nd.valid) {
                std::snprintf(msg, sizeof(msg),
                              "  of which stochastic %.3e (%.2fx the model) and fixed-pattern %.3e "
                              "(%.0f%% of the variance)%s",
                              nd.stochastic, nd.stochastic / nc.predicted_median, nd.fixed,
                              100.0 * nd.fixed_share,
                              nd.fixed_share > 0.3 ? " -- fixed pattern dominates: this is what flats remove." : ".");
                obs.message(msg);
            }
        }
    }

    result.stacked = std::move(stacked);
    result.noise_map = std::move(noise_map);
    result.measured_noise = std::move(measured_noise);
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
