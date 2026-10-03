#include "catch_amalgamated.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/io/image.hpp"
#include <cmath>
#include <filesystem>

using namespace nukex;

namespace {
// The old read_pixel, rebuilt on the range read, so the existing cases keep
// asserting what they always asserted. Test-only: production code reads by
// frame-row, which is the access pattern the layout is built for.
int gather_pixel(const FrameCache& cache, int x, int y, int ch,
                 float* out_values, std::uint8_t* out_valid = nullptr) {
    const int n = cache.n_frames_written();
    const int p = y * cache.width() + x;
    for (int f = 0; f < n; ++f) {
        std::uint8_t ok = 0;
        if (!cache.read_frame_range(f, p, 1, ch, out_values + f, &ok))
            return f;
        if (out_valid) out_valid[f] = ok;
    }
    return n;
}
} // namespace

TEST_CASE("FrameCache::encode/decode roundtrip", "[cache]") {
    // Test several values across [0, 1]
    float test_values[] = {0.0f, 0.001f, 0.25f, 0.5f, 0.75f, 0.999f, 1.0f};
    for (float v : test_values) {
        uint16_t encoded = FrameCache::encode(v);
        float decoded = FrameCache::decode(encoded);
        REQUIRE(decoded == Catch::Approx(v).margin(1.0f / 65535.0f));
    }
}

TEST_CASE("FrameCache::encode clamps to [0, 1]", "[cache]") {
    REQUIRE(FrameCache::encode(-0.1f) == 0);
    REQUIRE(FrameCache::encode(1.5f) == 65535);
}

TEST_CASE("FrameCache layout is frame-major", "[frame_cache]") {
    // Why this is asserted rather than left implicit: under the old
    // pixel-major index, consecutive writes for ONE frame were max_frames
    // elements apart, so writing a 66 MB frame dirtied every page of a
    // 10.4 GB mapping. Measured from /proc/diskstats on a 156-frame 24 MP
    // run: 1,704 MB written per frame, 26x amplification, and Phase A's
    // per-frame cost climbing 3.85 s -> 13.4 s as the cache filled.
    const int W = 5, H = 3, NC = 3;

    // One frame occupies exactly W*H*NC consecutive elements.
    REQUIRE(FrameCache::element_offset(W, H, NC, 0, 0, 0, 0) == 0);
    REQUIRE(FrameCache::element_offset(W, H, NC, 0, 0, 0, 1)
                == static_cast<std::size_t>(W) * H * NC);

    // Within a frame: channel is fastest, then x, then y.
    REQUIRE(FrameCache::element_offset(W, H, NC, 0, 0, 1, 0) == 1);
    REQUIRE(FrameCache::element_offset(W, H, NC, 1, 0, 0, 0) == NC);
    REQUIRE(FrameCache::element_offset(W, H, NC, 0, 1, 0, 0) == W * NC);

    // The index does not depend on max_frames at all, which is the property
    // that makes a frame's writes sequential regardless of batch depth.
    REQUIRE(FrameCache::element_offset(W, H, NC, 3, 2, 1, 4)
                == 4 * static_cast<std::size_t>(W) * H * NC
                 + (2 * static_cast<std::size_t>(W) + 3) * NC + 1);

    // And a whole frame-row of one channel is a single strided span:
    // consecutive pixels are exactly n_channels apart.
    REQUIRE(FrameCache::element_offset(W, H, NC, 1, 0, 2, 7)
                - FrameCache::element_offset(W, H, NC, 0, 0, 2, 7) == NC);
}

TEST_CASE("FrameCache: write and read back single frame", "[cache]") {
    Image frame(4, 4, 1);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            frame.at(x, y, 0) = (x + y * 4) / 16.0f;

    FrameCache cache(4, 4, 1, 10, "/tmp");
    cache.write_frame(frame, 0);
    REQUIRE(cache.n_frames_written() == 1);

    float values[10];
    int n = gather_pixel(cache, 2, 1, 0, values);
    REQUIRE(n == 1);
    // Pixel (2, 1): value = (2 + 1*4) / 16 = 0.375
    REQUIRE(values[0] == Catch::Approx(0.375f).margin(0.001f));
}

TEST_CASE("FrameCache: write multiple frames, read all back", "[cache]") {
    Image f1(8, 8, 2);
    Image f2(8, 8, 2);
    Image f3(8, 8, 2);
    f1.fill(0.3f);
    f2.fill(0.5f);
    f3.fill(0.7f);

    FrameCache cache(8, 8, 2, 10, "/tmp");
    cache.write_frame(f1, 0);
    cache.write_frame(f2, 1);
    cache.write_frame(f3, 2);
    REQUIRE(cache.n_frames_written() == 3);

    float values[10];
    int n = gather_pixel(cache, 4, 4, 0, values);
    REQUIRE(n == 3);
    REQUIRE(values[0] == Catch::Approx(0.3f).margin(0.001f));
    REQUIRE(values[1] == Catch::Approx(0.5f).margin(0.001f));
    REQUIRE(values[2] == Catch::Approx(0.7f).margin(0.001f));

    // Channel 1 should also work
    n = gather_pixel(cache, 4, 4, 1, values);
    REQUIRE(n == 3);
    REQUIRE(values[0] == Catch::Approx(0.3f).margin(0.001f));
}

TEST_CASE("FrameCache: temp file cleaned up on destruction", "[cache]") {
    std::string filepath;
    {
        FrameCache cache(2, 2, 1, 1, "/tmp");
        Image frame(2, 2, 1);
        frame.fill(0.5f);
        cache.write_frame(frame, 0);
        // We can't easily get the filepath, but verify no crash on destruction
    }
    // Cache destroyed -- temp file should be gone
    // (No way to verify path externally without exposing it, but no crash = success)
}

TEST_CASE("FrameCache: quantization error within tolerance", "[cache]") {
    // Verify max quantization error is < 2/65535 ~ 3e-5
    Image frame(16, 16, 1);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            frame.at(x, y, 0) = (x * 16 + y) / 256.0f;

    FrameCache cache(16, 16, 1, 1, "/tmp");
    cache.write_frame(frame, 0);

    float values[1];
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            gather_pixel(cache, x, y, 0, values);
            float original = frame.at(x, y, 0);
            REQUIRE(std::fabs(values[0] - original) < 2.0f / 65535.0f);
        }
    }
}

TEST_CASE("FrameCache: a sparse global frame set reads back only what it received",
          "[frame_cache]") {
    // The defect this replaced: write_frame stored at the GLOBAL batch index
    // and n_frames_written_ became index+1, while read_pixel returns slots
    // 0..n-1 contiguously. A cache given frames 5, 9 and 12 of a twenty-frame
    // batch therefore reported thirteen frames and handed Phase B ten
    // unwritten slots as though they were measurements. Any batch producing
    // more than one cache hit this, not only mono ones.
    const std::string dir = "/tmp";
    FrameCache cache(2, 2, 1, /*max_frames*/ 20, dir);

    const int globals[] = {5, 9, 12};
    const float values[] = {0.25f, 0.50f, 0.75f};
    for (int i = 0; i < 3; i++) {
        Image f(2, 2, 1);
        f.fill(values[i]);
        const int local = cache.write_frame(f, globals[i]);
        REQUIRE(local == i);            // dense, in write order
    }

    REQUIRE(cache.n_frames_written() == 3);   // not 13

    float out[20] = {0};
    const int n = gather_pixel(cache, 0, 0, 0, out);
    REQUIRE(n == 3);
    for (int i = 0; i < 3; i++)
        REQUIRE(out[i] == Catch::Approx(values[i]).margin(1e-4));

    // And the map back to the batch's own numbering survives, which is what
    // lets Phase B find each frame's FrameStats.
    REQUIRE(cache.global_frame(0) == 5);
    REQUIRE(cache.global_frame(1) == 9);
    REQUIRE(cache.global_frame(2) == 12);
    REQUIRE(cache.global_frame(3) == -1);
}

TEST_CASE("FrameCache: coverage survives the round trip", "[frame_cache]") {
    // Phase B reads its per-frame samples back from the cache, so coverage has
    // to live HERE and not only in the Phase A accumulators. Storing a warped
    // zero without recording that it is absent is what left a rim at 94% of
    // interior brightness on a finished stack even after the accumulators
    // themselves were guarded.
    FrameCache cache(2, 2, 1, /*max_frames*/ 4, "/tmp");

    Image f(2, 2, 1);
    f.fill(0.5f);

    // Frame 0 covers everything.
    cache.write_frame(f, 0);

    // Frame 1 covers only pixel (0,0).
    CoverageMask partial(2, 2, 1);
    partial.set_covered(0, 0, 0, true);
    cache.write_frame(f, 1, partial);

    float vals[4] = {0};
    std::uint8_t ok[4] = {9, 9, 9, 9};

    int n = gather_pixel(cache, 0, 0, 0, vals, ok);
    REQUIRE(n == 2);
    REQUIRE(ok[0] == 1);   // frame 0: covered
    REQUIRE(ok[1] == 1);   // frame 1: covered here

    n = gather_pixel(cache, 1, 1, 0, vals, ok);
    REQUIRE(n == 2);
    REQUIRE(ok[0] == 1);   // frame 0 covered everything
    REQUIRE(ok[1] == 0);   // frame 1 did NOT cover this pixel
    // The stored value is still 0.5 -- indistinguishable from real data
    // without the mask, which is the entire point.
    REQUIRE(vals[1] == Catch::Approx(0.5f).margin(1e-4));

    // An omitted mask means "cloned, not warped": covers itself completely.
    REQUIRE(gather_pixel(cache, 1, 0, 0, vals, ok) == 2);
    REQUIRE(ok[0] == 1);
}

TEST_CASE("FrameCache: writeback covers the frame just written, and only it",
          "[frame_cache]") {
    // Frame-major makes one frame's dirty pages a contiguous range, so the
    // sync goes back to per-frame while touching that frame's 66 MB instead
    // of the whole 10.4 GB mapping. That is a TIGHTER dirty-page bound than
    // syncing every 8th frame, which is what the pixel-major layout forced.
    //
    // The bound matters: on 2026-09-05 a 33-frame 24 MP OSC cache held 5.2 GB
    // of dirty page cache beside a 14.8 GB voxel cube and PixInsight was
    // OOM-killed mid-cache.
    Image frame(8, 8, 1);
    FrameCache cache(8, 8, 1, 64, "/tmp");
    for (int f = 0; f < 32; ++f) {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                frame.at(x, y, 0) = static_cast<float>((f + x + y) % 16) / 16.0f;
        cache.write_frame(frame, f);
    }
    REQUIRE(cache.sync_count() == 32);   // one per frame, not one per eight

    // Every frame still reads back exactly. mmap is coherent within the
    // process, so writeback scheduling cannot change what Phase B sees.
    float values[64];
    const int n = gather_pixel(cache, 3, 5, 0, values);
    REQUIRE(n == 32);
    for (int f = 0; f < 32; ++f)
        REQUIRE(values[f] ==
                Catch::Approx(static_cast<float>((f + 3 + 5) % 16) / 16.0f).margin(1e-4));

    // An explicit flush is still available for the end of Phase A.
    const int before = cache.sync_count();
    cache.flush();
    REQUIRE(cache.sync_count() == before + 1);
}

TEST_CASE("FrameCache::read_frame_range crosses a row boundary",
          "[frame_cache]") {
    // The only test that started a range read mid-row and let it run into
    // the next row was "read_frame_range agrees with read_pixel exactly",
    // deleted when the frame-major layout change removed read_pixel --
    // gather_pixel is built ON read_frame_range, so it could no longer serve
    // as an independent check of that property. Nothing else in the suite
    // covers it: test_shadow_buffers.cpp uses start_voxel 0 and 4 (with
    // W=4), both row-aligned. Production does NOT restrict callers to
    // row-aligned starts -- gpu_shadow_buffers.cpp:118 hands read_frame_range
    // an arbitrary start_voxel taken straight from an arbitrary batch
    // boundary. This exercises the specialised base+stride read path added
    // in the layout change (not just element_offset's arithmetic) against
    // values computed independently here.
    const int W = 5, H = 3, NC = 2, NF = 3;
    FrameCache cache(W, H, NC, /*max_frames*/ NF, "/tmp");

    // Every (x, y, ch, f) gets its own value and its own coverage bit, so a
    // read that lands on the wrong pixel, channel, or frame reads back a
    // detectably wrong answer rather than a coincidental match.
    auto value_of = [&](int x, int y, int ch, int f) {
        const int index = ((f * H + y) * W + x) * NC + ch;
        return static_cast<float>(index) / 100.0f;   // unique, in [0, 0.89]
    };
    auto covered_of = [&](int x, int y, int ch, int f) {
        return ((x + 2 * y + ch + f) % 3) != 0;
    };

    for (int f = 0; f < NF; ++f) {
        Image img(W, H, NC);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int ch = 0; ch < NC; ++ch)
                    img.at(x, y, ch) = value_of(x, y, ch, f);
        CoverageMask cov(W, H, NC);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int ch = 0; ch < NC; ++ch)
                    cov.set_covered(ch, y, x, covered_of(x, y, ch, f));
        cache.write_frame(img, f, cov);
    }

    // start_pixel=7 with W=5 is (x=2, y=1) -- W does not divide 7 -- and the
    // 6-pixel run ends at p=12, (x=2, y=2): it crosses the row 1/row 2
    // boundary mid-row on both ends. Frame 2 (of 3) and channel 1 (of 2) are
    // both non-zero indices, so the frame and channel strides are genuinely
    // exercised, not just the pixel stride.
    const int start_pixel = 7, count = 6, ch = 1, f = 2;
    REQUIRE(start_pixel % W != 0);
    REQUIRE(start_pixel / W != (start_pixel + count - 1) / W);

    std::vector<float>        vals(count);
    std::vector<std::uint8_t> ok(count);
    REQUIRE(cache.read_frame_range(f, start_pixel, count, ch,
                                   vals.data(), ok.data()));

    for (int i = 0; i < count; ++i) {
        const int p = start_pixel + i;
        const int x = p % W, y = p / W;
        REQUIRE(vals[i] == Catch::Approx(value_of(x, y, ch, f)).margin(1e-4));
        REQUIRE(ok[i] == (covered_of(x, y, ch, f) ? 1 : 0));
    }
}

TEST_CASE("FrameCache::read_frame_range refuses ranges it cannot serve",
          "[frame_cache]") {
    // Loud and total, never partial: a caller that silently got half a row
    // would fit a distribution to whatever was left in its buffer.
    const int W = 4, H = 2;
    FrameCache cache(W, H, 1, /*max_frames*/ 4, "/tmp");
    Image img(W, H, 1);
    img.fill(0.5f);
    cache.write_frame(img, 0);

    std::vector<float> row(16, -1.0f);
    REQUIRE_FALSE(cache.read_frame_range(1, 0, W * H, 0, row.data(), nullptr));
    REQUIRE_FALSE(cache.read_frame_range(-1, 0, W * H, 0, row.data(), nullptr));
    REQUIRE_FALSE(cache.read_frame_range(0, 0, W * H + 1, 0, row.data(), nullptr));
    REQUIRE_FALSE(cache.read_frame_range(0, W * H - 2, 4, 0, row.data(), nullptr));
    REQUIRE_FALSE(cache.read_frame_range(0, -1, 2, 0, row.data(), nullptr));
    REQUIRE_FALSE(cache.read_frame_range(0, 0, 2, 1, row.data(), nullptr));  // no ch 1
    for (float v : row) REQUIRE(v == -1.0f);   // nothing was written
}
