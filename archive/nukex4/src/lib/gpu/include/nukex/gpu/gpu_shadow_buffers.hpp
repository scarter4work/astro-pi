#pragma once

#include "nukex/gpu/gpu_config.hpp"
#include "nukex/core/voxel.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/frame_stats.hpp"
#include "nukex/classify/weight_computer.hpp"
#include <vector>
#include <cstdint>

namespace nukex {

class FrameCache;  // Forward declare

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
    std::vector<uint16_t> n_frames;         // [batch]

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
    /// Reads pixel values from the frame cache.
    /// start_voxel: linear index into the cube's voxel array.
    /// count: number of voxels in this batch.
    void extract_from_cube(const Cube& cube, const FrameCache& cache,
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
