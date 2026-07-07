#pragma once

#include "nukex/gpu/gpu_config.hpp"
#include "nukex/core/voxel.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/classify/weight_computer.hpp"
#include "nukex/stacker/cache_sig.hpp"
#include <vector>
#include <cstdint>

namespace nukex {

/// Structure-of-Arrays shadow buffers for GPU transfer.
///
/// Voxel data is AoS (array of structs). GPU prefers SoA for coalesced
/// memory access. These buffers serve as the transient projection between
/// the voxel (system of record) and GPU kernels.
///
/// Memory layout: channel-major within each field.
///   welford_mean[ch * batch + voxel_idx]
///   pixel_values[ch * max_frames * batch + frame * batch + voxel_idx]
///
/// This ensures adjacent work-items (processing adjacent voxels)
/// access adjacent memory locations = coalesced reads.
struct ShadowBuffers {
    int batch_size   = 0;
    int n_channels   = 0;
    int max_frames   = 0;  // The actual N for this batch (from voxel n_frames)

    // ── Inputs (host → device) ────────────────────────────────────────
    std::vector<float>    welford_mean;     // [n_ch * batch]
    std::vector<float>    welford_M2;       // [n_ch * batch]
    std::vector<uint32_t> welford_n;        // [n_ch * batch]
    std::vector<float>    pixel_values;     // [n_ch * max_frames * batch]
    std::vector<uint16_t> n_frames;         // [batch] — per-voxel UNION frame
                                            // count (liveness gate only; not a
                                            // per-channel loop bound anymore)

    // ── Per-channel real-sample accounting (heterogeneous-geometry fix) ──
    //
    // Phase B's per-voxel `n_frames` scalar is the UNION of frame counts
    // across all channels; for a heterogeneous batch (e.g. mono-L + debayered
    // OSC) it exceeds any single channel's real sample count and, used as a
    // uniform kernel loop bound, walks `fi` past channel `ch`'s data into
    // channel `ch+1` (aliasing) or out of bounds. These two arrays replace it
    // with correct PER-CHANNEL accounting.
    //
    // channel_n_frames[ch]      = number of real (written) frames feeding
    //                             channel ch. Uniform across all voxels in the
    //                             batch (it is a property of the slot's cache,
    //                             not the voxel). This is the correct loop
    //                             bound for kernel per-channel inner loops.
    //
    // channel_frame_remap[ch*N+k] = the GLOBAL frame index of channel ch's
    //                             k-th real sample, so kernels index the flat
    //                             per-frame frame_stats[] as
    //                             frame_stats[remap[ch*N+fi]] instead of the
    //                             (now-wrong) frame_stats[fi].
    //
    // Defaults from allocate(): channel_n_frames[ch] = max_frames and
    // channel_frame_remap[ch*N+k] = k (identity). That makes buffers built
    // directly by tests (which populate `pixel_values`/`n_frames` densely at
    // [0..N-1] and never call extract_from_cube) behave byte-identically to
    // the pre-fix per-voxel loops. extract_from_cube() overwrites both from
    // the slot's cache for the real Phase B path.
    std::vector<uint16_t> channel_n_frames;   // [n_ch]
    std::vector<int32_t>  channel_frame_remap; // [n_ch * max_frames]

    // ── Intermediate (persist on device across kernel passes) ─────────
    std::vector<float>    pixel_weights;    // [n_ch * max_frames * batch]

    // ── Classification output (device → host) ─────────────────────────
    std::vector<uint16_t> cloud_frame_count;  // [batch]
    std::vector<uint16_t> trail_frame_count;  // [batch]
    std::vector<float>    worst_sigma_score;  // [batch]
    std::vector<float>    best_sigma_score;   // [batch]
    std::vector<float>    mean_weight_out;    // [batch]
    std::vector<float>    total_exposure_out; // [batch]

    // ── Robust stats output (device → host) ───────────────────────────
    std::vector<float>    mad_out;            // [n_ch * batch]
    std::vector<float>    biweight_midvar_out;// [n_ch * batch]
    std::vector<float>    iqr_out;            // [n_ch * batch]

    // ── Distribution input (host → device, after CPU fitting) ─────────
    std::vector<float>    dist_true_signal;   // [n_ch * batch]
    std::vector<float>    dist_uncertainty;   // [n_ch * batch]
    std::vector<float>    dist_confidence;    // [n_ch * batch]

    /// Whether the Phase B model-selection fit converged for this
    /// voxel-channel (1 = converged, 0 = did not — e.g. KDEFitter's
    /// hard n<3 floor for sparse-coverage voxels). Populated by
    /// extract_distributions() from ZDistribution::shape (UNKNOWN means
    /// no fitter converged; every converged fitter sets a real shape).
    /// Defaults to 1 (converged) on allocate() so buffers built directly
    /// by tests/other callers that never populate this field keep the
    /// pre-existing "trust dist_true_signal" behaviour.
    /// select_pixels() falls back to the median of the raw per-frame
    /// samples when this is 0, instead of silently emitting the
    /// zeroed/default true_signal_estimate.
    std::vector<uint8_t>  dist_converged;     // [n_ch * batch]

    // ── Selection output (device → host) ──────────────────────────────
    std::vector<float>    output_value;       // [n_ch * batch]
    std::vector<float>    noise_sigma;        // [n_ch * batch]
    std::vector<float>    snr_out;            // [n_ch * batch]

    /// Running count of voxel-channels that hit the !converged median
    /// fallback in select_pixels(), accumulated across every batch
    /// processed against this buffer since the last allocate(). Reset
    /// to 0 by allocate(); observability hook so Phase B can report
    /// sparse-coverage fallback usage instead of it being silent.
    std::int64_t           low_n_fallback_count = 0;

    /// Allocate all buffers for a given batch size.
    void allocate(int batch_size, int n_channels, int max_frames);

    /// Extract voxel data from the cube into SoA layout.
    /// Reads per-frame pixel values via slot_refs (one entry per cube slot).
    /// start_voxel: linear index into the cube's voxel array.
    /// count: number of voxels in this batch.
    void extract_from_cube(const Cube& cube,
                           const std::vector<ChannelCacheRef>& slot_refs,
                           int start_voxel, int count, int n_channels);

    /// Write classification + robust stats back to voxels.
    void writeback_classification(Cube& cube, int start_voxel, int count,
                                   int n_channels) const;

    /// Write fitted distributions from voxels into the dist_* input buffers.
    /// Called after CPU fitting, before the select_pixels kernel.
    void extract_distributions(const Cube& cube, int start_voxel, int count,
                                int n_channels);

    /// Write selection output (value, noise, SNR) back to the cube's output arrays.
    void writeback_selection(Cube& cube, int start_voxel, int count,
                              int n_channels,
                              float* output_image, float* noise_image) const;
};

} // namespace nukex
