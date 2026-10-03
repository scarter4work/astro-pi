#include "catch_amalgamated.hpp"
#include "nukex/gpu/gpu_shadow_buffers.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/stacker/cache_sig.hpp"
#include "nukex/core/cube.hpp"
#include "nukex/core/coverage_mask.hpp"
#include "nukex/io/image.hpp"
#include <cstdint>
#include <string>

using namespace nukex;

namespace {

constexpr int W = 4;
constexpr int H = 2;
constexpr int NPIX = W * H;

// A value that is unique in (frame, pixel, channel), so a transposed or
// off-by-one index cannot accidentally produce the right answer.
float sample_of(int f, int x, int y, int ch) {
    return 0.05f * static_cast<float>(f)
         + 0.01f * static_cast<float>(y * W + x)
         + 0.30f * static_cast<float>(ch);
}

ChannelConfig lrgb_config() {
    ChannelConfig cc;
    cc.n_channels = 4;
    cc.channel_names[0] = "R";
    cc.channel_names[1] = "G";
    cc.channel_names[2] = "B";
    cc.channel_names[3] = "L";
    return cc;
}

} // namespace

TEST_CASE("extract_from_cube: DIRECT, luma synthesis, coverage and ragged "
          "frame counts", "[shadow_buffers]") {
    // The gate on the frame-major cache layout. Every assertion below is an
    // exact value derived from sample_of(), not a recorded hash, so it holds
    // whatever order the cache stores its bytes in.

    // ── A 3-channel (OSC) cache with 3 frames ────────────────────────────
    FrameCache osc(W, H, 3, /*max_frames*/ 8, "/tmp");
    for (int f = 0; f < 3; ++f) {
        Image img(W, H, 3);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    img.at(x, y, ch) = sample_of(f, x, y, ch);

        if (f == 2) {
            // Frame 2 was warped and only covers the left half, and only in
            // channels 0 and 1. Per-channel coverage is the real shape: channel
            // registration can push one plane off the source while others stay.
            CoverageMask cov(W, H, 3);
            for (int ch = 0; ch < 2; ++ch)
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W / 2; ++x)
                        cov.set_covered(ch, y, x, true);
            osc.write_frame(img, f, cov);
        } else {
            osc.write_frame(img, f);   // empty mask == covers itself completely
        }
    }
    REQUIRE(osc.n_frames_written() == 3);

    // ── Slot routing: R, G, B direct; L synthesised from the same cache ──
    std::vector<ChannelCacheRef> refs(4);
    refs[0] = {&osc, 0, SlotSynthesis::DIRECT};
    refs[1] = {&osc, 1, SlotSynthesis::DIRECT};
    refs[2] = {&osc, 2, SlotSynthesis::DIRECT};
    refs[3] = {&osc, -1, SlotSynthesis::REC709_LUMA};

    Cube cube(W, H, lrgb_config());

    const int C = 4, N = 3, B = NPIX;
    ShadowBuffers buf;
    buf.allocate(B, C, N);
    buf.extract_from_cube(cube, refs, /*start_voxel*/ 0, /*count*/ B, C);

    for (int vi = 0; vi < B; ++vi) {
        const int x = vi % W, y = vi / W;
        for (int fi = 0; fi < N; ++fi) {
            for (int ch = 0; ch < 3; ++ch) {
                const float got = buf.pixel_values[ch * N * B + fi * B + vi];
                REQUIRE(got == Catch::Approx(sample_of(fi, x, y, ch))
                                   .margin(2.0f / 65535.0f));
                const bool covered = (fi != 2) || (ch < 2 && x < W / 2);
                REQUIRE(buf.sample_valid(ch, fi, vi, B) == covered);
            }

            // rec709 luma, exactly as Phase A accumulates it.
            const float want = 0.299f * sample_of(fi, x, y, 0)
                             + 0.587f * sample_of(fi, x, y, 1)
                             + 0.114f * sample_of(fi, x, y, 2);
            REQUIRE(buf.pixel_values[3 * N * B + fi * B + vi]
                        == Catch::Approx(want).margin(2.0f / 65535.0f));
            // Synthetic luminance mixes all three planes, so it is a
            // measurement only where all three are.
            const bool luma_ok = (fi != 2);
            REQUIRE(buf.sample_valid(3, fi, vi, B) == luma_ok);
        }
        for (int ch = 0; ch < C; ++ch)
            REQUIRE(buf.n_frames[ch * B + vi] == 3);
    }
}

TEST_CASE("extract_from_cube: two caches of different length keep their own "
          "frame counts", "[shadow_buffers]") {
    // An L24/R12 batch: each slot reads its own cache, and a short channel
    // must not be padded out to the long one's length. Fitting the whole row
    // would average the short channel's samples against zero padding.
    FrameCache cache_l(W, H, 1, /*max_frames*/ 8, "/tmp");
    FrameCache cache_r(W, H, 1, /*max_frames*/ 8, "/tmp");

    for (int f = 0; f < 4; ++f) {
        Image img(W, H, 1);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) img.at(x, y, 0) = sample_of(f, x, y, 0);
        cache_l.write_frame(img, f);
    }
    for (int f = 0; f < 2; ++f) {
        Image img(W, H, 1);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) img.at(x, y, 0) = sample_of(f, x, y, 1);
        cache_r.write_frame(img, f);
    }

    ChannelConfig cc;
    cc.n_channels = 2;
    cc.channel_names[0] = "L";
    cc.channel_names[1] = "R";
    Cube cube(W, H, cc);

    std::vector<ChannelCacheRef> refs(2);
    refs[0] = {&cache_l, 0, SlotSynthesis::DIRECT};
    refs[1] = {&cache_r, 0, SlotSynthesis::DIRECT};

    const int C = 2, N = 4, B = NPIX;   // N is the WIDEST set, not a count
    ShadowBuffers buf;
    buf.allocate(B, C, N);
    buf.extract_from_cube(cube, refs, 0, B, C);

    for (int vi = 0; vi < B; ++vi) {
        const int x = vi % W, y = vi / W;
        REQUIRE(buf.n_frames[0 * B + vi] == 4);
        REQUIRE(buf.n_frames[1 * B + vi] == 2);
        for (int fi = 0; fi < 4; ++fi)
            REQUIRE(buf.pixel_values[0 * N * B + fi * B + vi]
                        == Catch::Approx(sample_of(fi, x, y, 0))
                               .margin(2.0f / 65535.0f));
        for (int fi = 0; fi < 2; ++fi)
            REQUIRE(buf.pixel_values[1 * N * B + fi * B + vi]
                        == Catch::Approx(sample_of(fi, x, y, 1))
                               .margin(2.0f / 65535.0f));
        // Slots 2 and 3 of R's row were never written and must stay zero.
        for (int fi = 2; fi < 4; ++fi)
            REQUIRE(buf.pixel_values[1 * N * B + fi * B + vi] == 0.0f);

        // And each local slot still maps back to its own cache's global frame.
        REQUIRE(buf.global_frame_of[0 * N + 3] == 3);
        REQUIRE(buf.global_frame_of[1 * N + 1] == 1);
        REQUIRE(buf.global_frame_of[1 * N + 2] == -1);
    }
}

TEST_CASE("extract_from_cube: a partial final batch addresses coverage with "
          "count, not batch_size", "[shadow_buffers]") {
    // The last batch of a cube is usually partial. Coverage bits are indexed
    // with the batch's `count`, and using the allocated batch_size instead put
    // them at offsets the kernels never read -- which made OSC output vary run
    // to run once the batch count started depending on free memory.
    FrameCache osc(W, H, 1, /*max_frames*/ 4, "/tmp");
    Image img(W, H, 1);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) img.at(x, y, 0) = sample_of(0, x, y, 0);
    CoverageMask cov(W, H, 1);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) cov.set_covered(0, y, x, x % 2 == 0);
    osc.write_frame(img, 0, cov);

    ChannelConfig cc;
    cc.n_channels = 1;
    cc.channel_names[0] = "L";
    Cube cube(W, H, cc);

    std::vector<ChannelCacheRef> refs(1);
    refs[0] = {&osc, 0, SlotSynthesis::DIRECT};

    const int C = 1, N = 1;
    ShadowBuffers buf;
    buf.allocate(/*batch_size*/ NPIX, C, N);

    // Second half only: voxels 4..7, a count of 4 against a batch_size of 8.
    const int start = 4, count = 4;
    buf.extract_from_cube(cube, refs, start, count, C);
    for (int vi = 0; vi < count; ++vi) {
        const int x = (start + vi) % W, y = (start + vi) / W;
        REQUIRE(buf.pixel_values[0 * N * count + 0 * count + vi]
                    == Catch::Approx(sample_of(0, x, y, 0))
                           .margin(2.0f / 65535.0f));
        REQUIRE(buf.sample_valid(0, 0, vi, count) == (x % 2 == 0));
    }
}
