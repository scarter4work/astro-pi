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
    std::vector<uint16_t> n_frames;         // [n_ch * batch]
    /// Batch-global frame index for each (channel, local slot), or -1 where
    /// the channel has no frame there. [n_ch * max_frames].
    ///
    /// Per-channel because channels no longer share a frame set: an LRGB-mono
    /// batch gives L 24 frames and R 12, from different caches, and the
    /// kernels must find each frame's FrameStats -- which are numbered
    /// globally -- from a local slot index.
    std::vector<int32_t>  global_frame_of;

    /// One bit per (channel, frame slot, voxel), same index as pixel_values:
    /// 1 when that frame actually covered that pixel, 0 when the warp left it
    /// outside the source.
    ///
    /// Bits rather than compaction, deliberately. Compacting the valid
    /// samples per voxel would make slot `fi` a DIFFERENT frame in different
    /// voxels, and frame_stats is indexed by slot -- every weight, gain and
    /// read-noise would then be attributed to the wrong exposure exactly at
    /// the coverage boundary, which is the region this exists to fix.
    /// The plane costs C*N/8 bytes per voxel, about 1% of the record.
    std::vector<uint8_t>  pixel_valid;

    /// `stride` must be the SAME voxel stride pixel_values was written with,
    /// which is the batch's `count` -- not `batch_size`. The final batch of a
    /// cube is usually partial, so the two differ, and using the member here
    /// put the coverage bits at offsets the kernels never read. Because the
    /// number of batches now depends on free memory, that made pixel output
    /// vary run to run on any cube large enough to need more than one batch:
    /// the two OSC corpora moved between otherwise identical runs while the
    /// single-batch mono ones stayed stable.
    bool sample_valid(int ch, int fi, int vi, int stride) const {
        if (pixel_valid.empty()) return true;   // no mask: everything covered
        const std::size_t b = (static_cast<std::size_t>(ch) * max_frames + fi)
                            * stride + vi;
        return (pixel_valid[b >> 3] >> (b & 7)) & 1u;
    }
    void set_sample_valid(int ch, int fi, int vi, bool v, int stride) {
        const std::size_t b = (static_cast<std::size_t>(ch) * max_frames + fi)
                            * stride + vi;
        const uint8_t bit = static_cast<uint8_t>(1u << (b & 7));
        if (v) pixel_valid[b >> 3] |= bit;
        else   pixel_valid[b >> 3] &= static_cast<uint8_t>(~bit);
    }

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

    // ── Selection output (device → host) ──────────────────────────────
    std::vector<float>    output_value;       // [n_ch * batch]
    std::vector<float>    noise_sigma;        // [n_ch * batch]
    std::vector<float>    snr_out;            // [n_ch * batch]

    /// Allocate all buffers for a given batch size.
    void allocate(int batch_size, int n_channels, int max_frames);

    /// Extract voxel data from the cube into SoA layout.
    /// Reads per-frame pixel values via slot_refs (one entry per cube slot).
    /// start_voxel: linear index into the cube's voxel array.
    /// count: number of voxels in this batch.
    void extract_from_cube(const Cube& cube,
                           const std::vector<ChannelCacheRef>& slot_refs,
                           int start_voxel, int count, int n_channels);

    /// Fill global_frame_of from the slot refs. Call once per batch, before
    /// extract_from_cube. Separate because it depends only on the caches,
    /// not on which voxels this batch covers.
    void map_frames(const std::vector<ChannelCacheRef>& slot_refs,
                    int n_channels);

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
