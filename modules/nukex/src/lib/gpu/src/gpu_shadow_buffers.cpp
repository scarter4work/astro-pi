#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/stacker/cache_sig.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace nukex {

void ShadowBuffers::allocate(int bs, int nc, int mf) {
    batch_size = bs;
    n_channels = nc;
    max_frames = mf;

    int B = bs, C = nc, N = mf;

    // Input buffers
    welford_mean.resize(C * B, 0.0f);
    welford_M2.resize(C * B, 0.0f);
    welford_n.resize(C * B, 0);
    pixel_values.resize(C * N * B, 0.0f);
    n_frames.resize(C * B, 0);
    // Deliberately NOT sized here. extract_from_cube builds it from the slot
    // refs; leaving it empty is the signal that the kernels are being driven
    // directly (unit tests), where a local slot index IS the global frame.

    // Intermediate
    pixel_weights.resize(C * N * B, 0.0f);
    // Left EMPTY on purpose, like global_frame_of: empty means "no coverage
    // information, everything is a real sample", which is what a unit test
    // driving the kernels directly means. extract_from_cube sizes it.

    // Classification output
    cloud_frame_count.resize(B, 0);
    trail_frame_count.resize(B, 0);
    worst_sigma_score.resize(B, 0.0f);
    best_sigma_score.resize(B, 0.0f);
    mean_weight_out.resize(B, 0.0f);
    total_exposure_out.resize(B, 0.0f);

    // Robust stats output
    mad_out.resize(C * B, 0.0f);
    biweight_midvar_out.resize(C * B, 0.0f);
    iqr_out.resize(C * B, 0.0f);

    // Distribution input
    dist_true_signal.resize(C * B, 0.0f);
    dist_uncertainty.resize(C * B, 0.0f);
    dist_confidence.resize(C * B, 0.0f);

    // Selection output
    output_value.resize(C * B, 0.0f);
    noise_sigma.resize(C * B, 0.0f);
    snr_out.resize(C * B, 0.0f);
}

void ShadowBuffers::extract_from_cube(
    const Cube& cube,
    const std::vector<ChannelCacheRef>& slot_refs,
    int start_voxel, int count, int nc) {

    int B = count;
    int C = nc;
    int N = max_frames;
    int w = cube.width;

    // Built here rather than by the caller so there is no ordering hazard:
    // the kernels cannot see pixel_values without also seeing the map that
    // says which global frame each local slot came from.
    map_frames(slot_refs, C);
    pixel_valid.assign((static_cast<std::size_t>(C) * N * B + 7) / 8, 0);

    // The cube's per-voxel statistics are indexed by voxel; the cache is read
    // by frame-row. Two loops rather than one, because interleaving them would
    // put the cache back on a per-pixel access pattern.
    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        const auto& voxel = cube.at(px, py);
        for (int ch = 0; ch < C; ch++) {
            welford_mean[ch * B + vi] = voxel.channel(ch).welford.mean;
            welford_M2[ch * B + vi]   = voxel.channel(ch).welford.M2;
            welford_n[ch * B + vi]    = voxel.channel(ch).welford.n;
        }
    }

    // Scratch for one frame-row of one channel. Sized by the BATCH, not by
    // the frame count, so this costs a few bytes per voxel however deep the
    // stack is -- which is what keeps it inside the host-RAM budget
    // estimate_batch_size was tuned to.
    std::vector<float>        row(static_cast<std::size_t>(B));
    std::vector<std::uint8_t> row_ok(static_cast<std::size_t>(B));
    std::vector<float>        g_row, b_row;
    std::vector<std::uint8_t> g_ok, b_ok;

    for (int ch = 0; ch < C; ch++) {
        // ref.cache == nullptr means no per-frame source for this slot (e.g.
        // an unmapped synthesised slot in a degenerate config). pixel_values
        // was zeroed by allocate(), so leaving it is correct -- distribution
        // fitting falls back to welford-only stats.
        if (ch >= static_cast<int>(slot_refs.size())) continue;
        const ChannelCacheRef& ref = slot_refs[ch];
        if (ref.cache == nullptr) continue;

        // The channel's OWN frame count, not the voxel's. They differ whenever
        // two slots read different caches.
        const int n_ch = std::min(ref.cache->n_frames_written(), N);

        if (ref.kind == SlotSynthesis::REC709_LUMA) {
            g_row.resize(B); b_row.resize(B);
            g_ok.resize(B);  b_ok.resize(B);
        }

        for (int fi = 0; fi < n_ch; fi++) {
            if (ref.kind == SlotSynthesis::DIRECT) {
                // A false return here is currently unreachable: n_ch above is
                // min(ref.cache->n_frames_written(), N), which is exactly what
                // read_frame_range gates its refusal on. It would be harmless
                // even if it did trip -- `continue` skips the memcpy and
                // set_sample_valid below, so the sample's valid bit stays at
                // the 0 pixel_valid was assigned to, and both Phase B
                // consumers gate on that bit.
                if (!ref.cache->read_frame_range(fi, start_voxel, B,
                                                 ref.cache_ch,
                                                 row.data(), row_ok.data()))
                    continue;

            } else if (ref.kind == SlotSynthesis::REC709_LUMA) {
                // Synthesise L per-frame from cached R, G, B. Same formula as
                // Phase A's per-pixel accumulation, which is what keeps the
                // distribution fitting consistent with the Welford stats.
                //
                // Same unreachable-but-harmless refusal as the DIRECT branch
                // above, for each of these three reads.
                if (!ref.cache->read_frame_range(fi, start_voxel, B, 0,
                                                 row.data(), row_ok.data()))
                    continue;
                if (!ref.cache->read_frame_range(fi, start_voxel, B, 1,
                                                 g_row.data(), g_ok.data()))
                    continue;
                if (!ref.cache->read_frame_range(fi, start_voxel, B, 2,
                                                 b_row.data(), b_ok.data()))
                    continue;
                for (int vi = 0; vi < B; ++vi) {
                    row[vi] = 0.299f * row[vi]
                            + 0.587f * g_row[vi]
                            + 0.114f * b_row[vi];
                    // Synthetic luminance mixes all three planes, so it is a
                    // measurement only where all three are.
                    row_ok[vi] = (row_ok[vi] && g_ok[vi] && b_ok[vi]) ? 1 : 0;
                }
            } else {
                // Defends against a future SlotSynthesis enumerator falling
                // through unhandled. Without this, `row`/`row_ok` would still
                // hold whatever the PREVIOUS (channel, frame) iteration left
                // in them, and the memcpy and set_sample_valid below would
                // copy that stale row into `dst` and mark it valid --
                // silently fitting a distribution to another channel's
                // pixels. Write nothing instead.
                continue;
            }

            float* dst = pixel_values.data()
                       + static_cast<std::size_t>(ch) * N * B
                       + static_cast<std::size_t>(fi) * B;
            std::memcpy(dst, row.data(), static_cast<std::size_t>(B) * sizeof(float));
            for (int vi = 0; vi < B; ++vi)
                set_sample_valid(ch, fi, vi, row_ok[vi] != 0, B);
        }

        for (int vi = 0; vi < B; ++vi)
            n_frames[ch * B + vi] = static_cast<uint16_t>(n_ch);
    }
}

void ShadowBuffers::map_frames(const std::vector<ChannelCacheRef>& slot_refs,
                               int nc) {
    const int C = nc, N = max_frames;
    global_frame_of.assign(C * N, -1);
    for (int ch = 0; ch < C && ch < static_cast<int>(slot_refs.size()); ch++) {
        const ChannelCacheRef& ref = slot_refs[ch];
        if (ref.cache == nullptr) continue;
        const int n = std::min(ref.cache->n_frames_written(), N);
        for (int fi = 0; fi < n; fi++)
            global_frame_of[ch * N + fi] = ref.cache->global_frame(fi);
    }
}

void ShadowBuffers::writeback_classification(
    Cube& cube, int start_voxel, int count, int nc) const {

    int B = count;
    int C = nc;
    int w = cube.width;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        auto& voxel = cube.at(px, py);

        voxel.cloud_frame_count = cloud_frame_count[vi];
        voxel.trail_frame_count = trail_frame_count[vi];
        voxel.worst_sigma_score = worst_sigma_score[vi];
        voxel.best_sigma_score  = best_sigma_score[vi];
        voxel.mean_weight       = mean_weight_out[vi];
        voxel.total_exposure    = total_exposure_out[vi];

        for (int ch = 0; ch < C; ch++) {
            voxel.channel(ch).mad                  = mad_out[ch * B + vi];
            voxel.channel(ch).biweight_midvariance = biweight_midvar_out[ch * B + vi];
            voxel.channel(ch).iqr                  = iqr_out[ch * B + vi];
        }
    }
}

void ShadowBuffers::extract_distributions(
    const Cube& cube, int start_voxel, int count, int nc) {

    int B = count;
    int C = nc;
    int w = cube.width;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        const auto& voxel = cube.at(px, py);

        for (int ch = 0; ch < C; ch++) {
            dist_true_signal[ch * B + vi] = voxel.channel(ch).distribution.true_signal_estimate;
            dist_uncertainty[ch * B + vi] = voxel.channel(ch).distribution.signal_uncertainty;
            dist_confidence[ch * B + vi]  = voxel.channel(ch).distribution.confidence;
        }
    }
}

void ShadowBuffers::writeback_selection(
    Cube& cube, int start_voxel, int count, int nc,
    float* output_image, float* noise_image) const {

    int B = count;
    int C = nc;
    int w = cube.width;
    int h = cube.height;

    for (int vi = 0; vi < B; vi++) {
        int voxel_idx = start_voxel + vi;
        int px = voxel_idx % w;
        int py = voxel_idx / w;
        auto& voxel = cube.at(px, py);

        for (int ch = 0; ch < C; ch++) {
            float val = output_value[ch * B + vi];
            float noise = noise_sigma[ch * B + vi];
            float snr = snr_out[ch * B + vi];

            voxel.channel(ch).snr = snr;

            // Write to output images (channel-by-channel, row-major)
            if (output_image)
                output_image[ch * w * h + py * w + px] = val;
            if (noise_image)
                noise_image[ch * w * h + py * w + px] = noise;
        }
    }
}

} // namespace nukex
