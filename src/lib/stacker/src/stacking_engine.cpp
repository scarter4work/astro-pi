#include "nukex/stacker/stacking_engine.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/channel_config.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/debayer.hpp"
#include "nukex/io/flat_calibration.hpp"
#include "nukex/alignment/frame_aligner.hpp"
#include "nukex/classify/weight_computer.hpp"
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

StackingEngine::StackingEngine(const Config& config) : config_(config) {}

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

StackingEngine::Result StackingEngine::execute(
    const std::vector<std::string>& light_paths,
    const std::vector<std::string>& flat_paths,
    ProgressObserver* progress)
{
    ProgressObserver& obs = progress ? *progress : null_progress_observer();
    Result result;
    if (light_paths.empty()) return result;

    // ═══ SETUP ═══════════════════════════════════════════════════════

    // Read first frame to determine dimensions and channel config
    auto first = FITSReader::read(light_paths[0]);
    if (!first.success) return result;

    int raw_width = first.image.width();
    int raw_height = first.image.height();

    // Detect Bayer pattern from FITS header
    BayerPattern bayer = parse_bayer_pattern(first.metadata.bayer_pattern);

    // Auto-detect channel config from metadata
    ChannelConfig ch_config;
    if (bayer != BayerPattern::NONE) {
        ch_config = ChannelConfig::from_mode(StackingMode::OSC_RGB);
        ch_config.bayer = bayer;  // Override with actual pattern from header
    } else {
        ch_config = ChannelConfig::from_mode(StackingMode::MONO_L);
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

        // 7. Accumulate into cube
        obs.advance(0, "  accumulating");
        for (int y = 0; y < out_height; y++) {
            for (int x = 0; x < out_width; x++) {
                auto& voxel = cube.at(x, y);
                for (int ch = 0; ch < n_ch; ch++) {
                    float value = aligned.image.at(x, y, ch);
                    voxel.welford[ch].update(value);
                    if (voxel.welford[ch].count() == 1) {
                        voxel.histogram[ch].initialize_range(
                            value - 0.1f, value + 0.1f);
                    }
                    voxel.histogram[ch].update(value);
                }
                voxel.n_frames++;
            }
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

    return result;
}

} // namespace nukex
