# Frame-Major Cache Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Store each cached frame's pixels contiguously so Phase A writes 66 MB
sequentially per frame instead of dirtying a 10.4 GB mapping, without changing
a single output pixel.

**Architecture:** `FrameCache`'s on-disk index changes from pixel-major
(`((y*W+x)*n_ch + ch)*max_frames + f`) to frame-major
(`f*(W*H*n_ch) + (y*W+x)*n_ch + ch`). The per-pixel Phase B read
(`read_pixel`, which gathers all frames at one pixel) is deleted and replaced
with a per-(frame, channel) range read that the batched Phase B extractor
already wants. The layout flip is deliberately the LAST behavioural change, so
that if a golden moves it is attributable to the layout alone.

**Tech Stack:** C++17, CMake, Catch2 v3 (amalgamated), POSIX `mmap`/`msync`,
OpenMP. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-07-phase-a-performance-design.md`
(section 4a is what this plan implements; sections 1-3 are the measurement
that motivates it)

## Global Constraints

- **Bit-identical output.** `make e2e` must PASS in verify mode against the
  committed goldens with **zero** regeneration. Not one hash may move —
  primaries or sweeps. If one moves, the change is wrong; do not re-cut a
  golden to make it green.
- **`test/fixtures/golden/lrgb_mono_ngc7635.json` is a FROZEN floor.** Its
  `golden_frozen: true` and its `golden_note` records why. Never regenerate it
  in this plan.
- The full unit suite must stay green: `ctest --output-on-failure` from the
  build directory. It was 78/78 at `338d6ab`; this plan adds tests, so the
  count rises — no test may go from passing to failing.
- **Never install into PixInsight by hand, and never `sudo` there.**
  `tools/run_e2e.sh` borrows the module into `/opt/PixInsight/bin` and removes
  it in an EXIT trap; that is the only sanctioned path. See
  `feedback_never_sudo_into_pixinsight`.
- Build commands, from the repo root:
  `cmake -S . -B build && cmake --build build -j$(nproc)`
- Frames are cached at `uint16` precision (`encode`/`decode`, +/-7.6e-6). Any
  new read path must decode identically — `stored * (1.0f/65535.0f)` — so that
  no value shifts by even one ULP.
- `read_pixel` must be **deleted**, not kept as a slow fallback. Leaving it
  invites an accidental O(n^2) caller. (Spec 4a, verbatim.)
- Commit after every task. Do not batch commits.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/lib/stacker/include/nukex/stacker/frame_cache.hpp` | Cache geometry, index arithmetic, read/write API | Modify — new `element_offset`, new `read_frame_range`, delete `read_pixel`, new `sync_range` |
| `src/lib/stacker/src/frame_cache.cpp` | Implementation | Modify — same |
| `src/lib/gpu/src/gpu_shadow_buffers.cpp` | Phase B extraction from cube + caches into SoA staging buffers | Modify — `extract_from_cube` reads per (frame, channel) range instead of per pixel |
| `test/unit/gpu/test_shadow_buffers.cpp` | Characterization of `extract_from_cube` — the real gate on this work | **Create** |
| `test/unit/stacker/test_frame_cache.cpp` | FrameCache unit tests | Modify — migrate off `read_pixel`, add layout and writeback tests |
| `test/CMakeLists.txt` | Test registration | Modify — one new `nukex_add_test` line |
| `docs/superpowers/specs/2026-09-07-phase-a-performance-design.md` | The design doc this implements | Modify (Task 6) — record measured result, tick 4a in section 5 |

`extract_from_cube` is the only consumer of `FrameCache`'s read path
(`grep -rn "read_pixel" src/` returns `frame_cache.cpp` and
`gpu_shadow_buffers.cpp` only), which is what makes this change containable.

---

### Task 1: Characterization test for `extract_from_cube`

This task adds **no production code**. It pins down exactly what Phase B
extracts today, so that Tasks 3 and 4 have a fast, precise gate instead of
waiting on a multi-hour E2E run. It must be written against the CURRENT
pixel-major code and pass immediately — that is the point of a
characterization test.

It covers all four behaviours the batched rewrite could plausibly break:
DIRECT reads, REC709_LUMA synthesis, per-sample coverage, and channels of
unequal length (two caches with different frame counts).

**Files:**
- Create: `test/unit/gpu/test_shadow_buffers.cpp`
- Modify: `test/CMakeLists.txt` (after line 122, beside the other gpu tests)

**Interfaces:**
- Consumes: `ShadowBuffers::allocate(int, int, int)`,
  `ShadowBuffers::extract_from_cube(const Cube&, const std::vector<ChannelCacheRef>&, int start_voxel, int count, int nc)`,
  `ShadowBuffers::sample_valid(int ch, int fi, int vi, int stride) const`,
  `FrameCache::write_frame(const Image&, int global_index, const CoverageMask& = {})`
- Produces: nothing consumed by later tasks except the test itself, which
  Tasks 3 and 4 re-run unchanged.

- [ ] **Step 1: Write the characterization test**

Create `test/unit/gpu/test_shadow_buffers.cpp`:

```cpp
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
```

- [ ] **Step 2: Register the test**

In `test/CMakeLists.txt`, immediately after the `test_gpu_agreement` line
(line 122), add:

```cmake
nukex_add_test(test_shadow_buffers unit/gpu/test_shadow_buffers.cpp nukex4_gpu nukex4_stacker)
```

- [ ] **Step 3: Build and run — it must PASS against the current code**

```bash
cmake -S . -B build && cmake --build build -j$(nproc) --target test_shadow_buffers
./build/test/test_shadow_buffers
```

Expected: **All tests passed** (3 test cases). This is a characterization
test, so a failure here means the test's expectations are wrong, not the
production code. Fix the test until it passes without touching `src/`.

- [ ] **Step 4: Run the whole suite to confirm nothing else moved**

```bash
cd build && ctest --output-on-failure
```

Expected: every test passes; the total is the previous count plus one.

- [ ] **Step 5: Commit**

```bash
git add test/unit/gpu/test_shadow_buffers.cpp test/CMakeLists.txt
git commit -m "test(gpu): characterize extract_from_cube before the layout change

The frame-major cache layout has to leave Phase B's staging buffers
bit-identical. Waiting on a four-corpus E2E run to find out is hours per
attempt, so pin the behaviour here instead: exact per-sample values rather
than a recorded hash, so the assertions hold whatever order the cache stores
its bytes in.

Covers what the batched rewrite could break -- DIRECT reads, rec709 luma
synthesis, per-channel coverage, ragged frame counts between two caches, and
a partial final batch addressing coverage with count rather than batch_size."
```

---

### Task 2: Add `read_frame_range` to FrameCache, still pixel-major

The new read primitive lands first and is proved **against the existing
`read_pixel`** while both are on the same layout. That equivalence is what
makes Task 4's layout flip a genuine single-variable change.

Granularity is one (frame, channel) over a contiguous run of pixels. That is
the smallest unit that is sequential under frame-major, and it is exactly what
`extract_from_cube` consumes: for OSC it reads `count` pixels at stride
`n_ch` within a single contiguous span, and for mono it is fully contiguous.
It also keeps the luma path's scratch at `count` floats rather than
`n_frames * count`, so the change costs nothing against the host-RAM budget
that `estimate_batch_size` was tuned to after the 2026-09-05 OOM.

**Files:**
- Modify: `src/lib/stacker/include/nukex/stacker/frame_cache.hpp`
- Modify: `src/lib/stacker/src/frame_cache.cpp`
- Test: `test/unit/stacker/test_frame_cache.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces, for Task 3 and Task 4:
  ```cpp
  // Reads frame slot `f`, channel `ch`, pixels [start_pixel, start_pixel+count)
  // in raster order. out_values[i] and out_valid[i] receive pixel start_pixel+i.
  // out_valid may be nullptr. Returns false (writing nothing) when f is not a
  // written slot or the range leaves the image.
  bool FrameCache::read_frame_range(int f, int start_pixel, int count, int ch,
                                    float* out_values,
                                    std::uint8_t* out_valid) const;
  ```

- [ ] **Step 1: Write the failing test**

Append to `test/unit/stacker/test_frame_cache.cpp`:

```cpp
TEST_CASE("FrameCache::read_frame_range agrees with read_pixel exactly",
          "[frame_cache]") {
    // read_frame_range is the batched read Phase B will use. Proving it
    // against read_pixel while BOTH are on the same layout is what makes the
    // later layout flip a single-variable change: if the goldens move after
    // it, the layout is the only thing that can have caused it.
    const int W = 5, H = 3, NC = 3, NF = 4;
    FrameCache cache(W, H, NC, /*max_frames*/ 8, "/tmp");

    for (int f = 0; f < NF; ++f) {
        Image img(W, H, NC);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                for (int ch = 0; ch < NC; ++ch)
                    img.at(x, y, ch) = 0.02f * static_cast<float>(f)
                                     + 0.01f * static_cast<float>(y * W + x)
                                     + 0.25f * static_cast<float>(ch);
        CoverageMask cov(W, H, NC);
        for (int ch = 0; ch < NC; ++ch)
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    cov.set_covered(ch, y, x, (x + y + f + ch) % 3 != 0);
        cache.write_frame(img, f, cov);
    }

    // Whole image, every channel, every frame.
    std::vector<float>        row(static_cast<std::size_t>(W) * H);
    std::vector<std::uint8_t> ok(static_cast<std::size_t>(W) * H);
    for (int ch = 0; ch < NC; ++ch) {
        for (int f = 0; f < NF; ++f) {
            REQUIRE(cache.read_frame_range(f, 0, W * H, ch,
                                           row.data(), ok.data()));
            for (int p = 0; p < W * H; ++p) {
                float ref_vals[8];
                std::uint8_t ref_ok[8];
                const int n = cache.read_pixel(p % W, p / W, ch,
                                               ref_vals, ref_ok);
                REQUIRE(n == NF);
                REQUIRE(row[p] == ref_vals[f]);      // exact, not Approx
                REQUIRE(ok[p]  == ref_ok[f]);
            }
        }
    }

    // A partial run in the middle, which is what a partial final batch does.
    REQUIRE(cache.read_frame_range(2, /*start_pixel*/ 4, /*count*/ 6, /*ch*/ 1,
                                   row.data(), ok.data()));
    for (int i = 0; i < 6; ++i) {
        float ref_vals[8];
        std::uint8_t ref_ok[8];
        cache.read_pixel((4 + i) % W, (4 + i) / W, 1, ref_vals, ref_ok);
        REQUIRE(row[i] == ref_vals[2]);
        REQUIRE(ok[i]  == ref_ok[2]);
    }

    // out_valid is optional.
    REQUIRE(cache.read_frame_range(0, 0, W * H, 0, row.data(), nullptr));
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
```

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build -j$(nproc) --target test_frame_cache
```

Expected: **compile error** — `'read_frame_range' is not a member of 'nukex::FrameCache'`.

- [ ] **Step 3: Declare it in the header**

In `src/lib/stacker/include/nukex/stacker/frame_cache.hpp`, directly after the
two `read_pixel` declarations, add:

```cpp
    /// Phase B: read one frame slot's channel over a contiguous pixel run.
    ///
    /// Pixels are addressed in raster order: `start_pixel` is `y*width + x`,
    /// and `out_values[i]` receives pixel `start_pixel + i` of frame slot `f`.
    /// `out_valid` may be null; when given it reports, per pixel, whether that
    /// frame actually covered it.
    ///
    /// This is the granularity Phase B batches at, and the granularity the
    /// storage layout is built for. Reading one pixel across all frames --
    /// what read_pixel did -- is the access pattern a frame-major layout is
    /// worst at, and it is why that method no longer exists.
    ///
    /// Returns false and writes NOTHING if `f` is not a written slot or the
    /// range leaves the image. Refusing totally rather than partially matters:
    /// a caller handed half a row would fit a distribution to whatever was
    /// left in its buffer.
    bool read_frame_range(int f, int start_pixel, int count, int ch,
                          float* out_values,
                          std::uint8_t* out_valid = nullptr) const;
```

- [ ] **Step 4: Implement it**

In `src/lib/stacker/src/frame_cache.cpp`, after the `read_pixel` overloads:

```cpp
bool FrameCache::read_frame_range(int f, int start_pixel, int count, int ch,
                                  float* out_values,
                                  std::uint8_t* out_valid) const {
    if (!mapped_ || out_values == nullptr) return false;
    if (ch < 0 || ch >= n_channels_) return false;
    if (count <= 0 || start_pixel < 0) return false;

    const int n_written = n_frames_written_.load(std::memory_order_acquire);
    if (f < 0 || f >= n_written) return false;

    const int64_t n_pixels = static_cast<int64_t>(width_) * height_;
    if (static_cast<int64_t>(start_pixel) + count > n_pixels) return false;

    // Addressed through offset() per pixel, which is correct under ANY
    // layout. That is deliberate: it is what lets the layout change in a
    // later commit without this function moving. Task 4 specialises it once
    // the stride is guaranteed.
    for (int i = 0; i < count; ++i) {
        const int p = start_pixel + i;
        const size_t e = offset(p % width_, p / width_, ch, f);
        out_values[i] = decode(mapped_[e]);
        // cov_bit() is defined as identical to offset(), so the coverage
        // plane stays in step with the values by construction.
        if (out_valid)
            out_valid[i] = static_cast<std::uint8_t>(
                (coverage_bits_[e >> 3] >> (e & 7)) & 1u);
    }
    return true;
}
```

- [ ] **Step 5: Run the tests**

```bash
cmake --build build -j$(nproc) --target test_frame_cache && ./build/test/test_frame_cache
```

Expected: **All tests passed**, including the two new cases.

- [ ] **Step 6: Run the full suite**

```bash
cd build && ctest --output-on-failure
```

Expected: all green.

- [ ] **Step 7: Commit**

```bash
git add src/lib/stacker/include/nukex/stacker/frame_cache.hpp \
        src/lib/stacker/src/frame_cache.cpp \
        test/unit/stacker/test_frame_cache.cpp
git commit -m "feat(stacker): read the cache a frame-row at a time

read_frame_range reads one frame slot's channel over a contiguous run of
pixels -- the granularity Phase B already batches at, and the one a
frame-major layout is built for. It lands here addressed through offset(), so
it is correct under either layout, and it is proved against read_pixel while
both are still on the old one. That equivalence is what makes the layout flip
a single-variable change.

It refuses a range it cannot serve totally rather than partially: a caller
handed half a row would fit a distribution to whatever was left in its
buffer."
```

---

### Task 3: Move Phase B's extraction onto the range read

Still pixel-major. The only thing changing is how `extract_from_cube` asks for
its data, so Task 1's characterization test is the complete gate.

**Files:**
- Modify: `src/lib/gpu/src/gpu_shadow_buffers.cpp:58-132` (`extract_from_cube`)
- Test: `test/unit/gpu/test_shadow_buffers.cpp` (unchanged — it must pass as written)

**Interfaces:**
- Consumes: `FrameCache::read_frame_range` from Task 2.
- Produces: nothing new. `pixel_values`, `pixel_valid`, `n_frames` and
  `global_frame_of` keep their exact existing layouts and contents.

- [ ] **Step 1: Rewrite `extract_from_cube`**

Replace the body of `ShadowBuffers::extract_from_cube` in
`src/lib/gpu/src/gpu_shadow_buffers.cpp` with:

```cpp
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
                if (!ref.cache->read_frame_range(fi, start_voxel, B,
                                                 ref.cache_ch,
                                                 row.data(), row_ok.data()))
                    continue;

            } else if (ref.kind == SlotSynthesis::REC709_LUMA) {
                // Synthesise L per-frame from cached R, G, B. Same formula as
                // Phase A's per-pixel accumulation, which is what keeps the
                // distribution fitting consistent with the Welford stats.
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
```

Three details that must not drift from the old code:

1. `n_frames` was previously `min(nf_read, N)` where `nf_read` came from
   `read_pixel`'s return, i.e. the cache's written count. `n_ch` is the same
   quantity, hoisted out of the pixel loop.
2. For `REC709_LUMA` the old code took `min(n_r, n_g, n_b)`. Three channels of
   one cache always have the same written count, so `n_ch` is that minimum;
   the `continue` guards keep it safe if a read is ever refused.
3. `GPU_MAX_FRAMES` stack arrays are gone. `N` is already capped at
   `GPU_MAX_FRAMES` by the caller (`gpu_executor.cpp:442`), so nothing
   changes — but the fixed-size buffers no longer sit on the stack.

- [ ] **Step 2: Build and run the characterization test unchanged**

```bash
cmake --build build -j$(nproc) --target test_shadow_buffers && ./build/test/test_shadow_buffers
```

Expected: **All tests passed** — the same 3 cases from Task 1, not edited.
If any assertion fails, the rewrite is wrong; fix the rewrite, never the test.

- [ ] **Step 3: Run the full suite**

```bash
cd build && ctest --output-on-failure
```

Expected: all green, including `test_gpu_agreement`, `test_gpu_cpu_fallback`
and the integration tests under `[.integration]` if they are enabled.

- [ ] **Step 4: Run the integration tests explicitly**

They are gated out of the default ctest run.

```bash
./build/test/integration/test_lrgb_mono "[integration]"
./build/test/integration/test_phase_a_router "[integration]"
./build/test/integration/test_phase_b_qsolve "[integration]"
```

Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/lib/gpu/src/gpu_shadow_buffers.cpp
git commit -m "refactor(gpu): extract Phase B staging a frame-row at a time

extract_from_cube asked the cache for one pixel across all frames, once per
voxel per channel. That is the access pattern a frame-major layout is worst
at, so it has to go before the layout can change.

Same data, same buffers, same order: the cube's per-voxel statistics keep
their voxel loop, and the per-frame samples now come from read_frame_range
over the batch's contiguous pixel run. The luma synthesis combines three
frame-rows instead of three pixels, with scratch sized by the batch rather
than by the frame count, so it costs a few bytes per voxel however deep the
stack is.

No output change -- the characterization test from the previous commit passes
unedited."
```

---

### Task 4: Flip the layout to frame-major and delete `read_pixel`

The behavioural change, isolated. Everything before this was a refactor under
the old layout.

**Files:**
- Modify: `src/lib/stacker/include/nukex/stacker/frame_cache.hpp`
- Modify: `src/lib/stacker/src/frame_cache.cpp`
- Test: `test/unit/stacker/test_frame_cache.cpp`

**Interfaces:**
- Consumes: `read_frame_range` from Task 2.
- Produces, for Task 5 and any future caller:
  ```cpp
  // Frame-major element index. Public and static so the layout is assertable
  // rather than inferred.
  static std::size_t FrameCache::element_offset(int width, int n_channels,
                                                int x, int y, int ch, int f);
  ```

- [ ] **Step 1: Write the failing layout test**

Add to `test/unit/stacker/test_frame_cache.cpp`:

```cpp
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
```

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build -j$(nproc) --target test_frame_cache
```

Expected: **compile error** — `'element_offset' is not a member of 'nukex::FrameCache'`.

- [ ] **Step 3: Change the layout**

In `src/lib/stacker/include/nukex/stacker/frame_cache.hpp`:

(a) Replace the class doc comment's layout paragraph:

```cpp
/// Disk-backed storage for aligned frames using memory-mapped uint16 encoding.
///
/// Frame-major layout: one frame's pixels are consecutive, so Phase A writes
/// each frame as a single sequential run and touches nothing else. The
/// previous pixel-major index put consecutive writes for one frame
/// max_frames elements apart, which meant writing a 66 MB frame dirtied every
/// page of a 10.4 GB mapping -- 26x write amplification measured on a
/// 156-frame 24 MP run, and the reason Phase A's per-frame cost climbed from
/// 3.85 s to 13.4 s as the cache filled.
///
/// Phase B reads it back a frame-row at a time (read_frame_range), which is
/// how it already batches: n_frames contiguous runs per batch rather than one
/// gather per pixel.
```

(b) Delete both `read_pixel` declarations and their doc comment entirely.

(c) Add the public static beside `encode`/`decode`:

```cpp
    /// Element index of (x, y, ch) in frame slot f.
    ///
    /// Public and static so the layout can be asserted directly rather than
    /// inferred from read-back values -- the storage order is the whole point
    /// of this class, and it should not be possible to change it silently.
    ///
    /// Note what is ABSENT: max_frames. The old index multiplied by it, which
    /// is why one frame's writes were spread across the entire mapping.
    static std::size_t element_offset(int width, int height, int n_channels,
                                      int x, int y, int ch, int f) {
        const std::size_t frame_elems =
            static_cast<std::size_t>(width) * height * n_channels;
        return static_cast<std::size_t>(f) * frame_elems
             + (static_cast<std::size_t>(y) * width + x) * n_channels
             + static_cast<std::size_t>(ch);
    }
```

(d) Replace the private `offset()` with a forwarder:

```cpp
    /// Element offset for pixel (x, y), channel ch, frame f.
    size_t offset(int x, int y, int ch, int f) const {
        return element_offset(width_, height_, n_channels_, x, y, ch, f);
    }
```

`cov_bit()` stays as `offset()` — the coverage plane keeps the same ordering
as the values by construction, which is what keeps them in step.

- [ ] **Step 4: Delete `read_pixel` and specialise the range read**

In `src/lib/stacker/src/frame_cache.cpp`, delete both `read_pixel` definitions
(the two-line forwarder and the full one). Nothing else in `src/` calls them —
confirm with:

```bash
grep -rn "read_pixel" src/
```

Expected: **no output.**

Then collect on the promise Task 2 deferred. `read_frame_range` addressed
through `offset()` per pixel so it would survive the layout change; now that
the layout guarantees a fixed stride, replace its loop:

```cpp
    // One frame's samples are consecutive, so a whole frame-row of one
    // channel is a single span at a stride of n_channels_ -- and stride 1 for
    // the mono case. Two integer divisions per pixel is what this replaces.
    const size_t base = offset(start_pixel % width_, start_pixel / width_, ch, f);
    const size_t stride = static_cast<size_t>(n_channels_);
    for (int i = 0; i < count; ++i) {
        const size_t e = base + static_cast<size_t>(i) * stride;
        out_values[i] = decode(mapped_[e]);
        if (out_valid)
            out_valid[i] = static_cast<std::uint8_t>(
                (coverage_bits_[e >> 3] >> (e & 7)) & 1u);
    }
    return true;
```

This is only correct because pixels are raster-contiguous within a frame,
which is exactly what the layout test in Step 1 asserts. The equivalence test
from Task 2 no longer compares against `read_pixel` (it is gone), so Step 5
migrates it — but the characterization test in Step 8 covers the same ground
end to end.

- [ ] **Step 5: Migrate the tests off `read_pixel`**

Six existing cases in `test/unit/stacker/test_frame_cache.cpp` use it. Each
becomes a `read_frame_range` call per frame. Add this helper near the top of
the file, after the `using namespace nukex;`:

```cpp
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
```

Then replace every `cache.read_pixel(` with `gather_pixel(cache, ` in this
file — the argument order and return value are identical. The affected cases:

- `FrameCache: write and read back single frame` (line ~35)
- `FrameCache: write multiple frames, read all back` (lines ~56, ~63)
- `FrameCache: quantization error within tolerance` (line ~94)
- `FrameCache: a sparse global frame set reads back only what it received` (line ~124)
- `FrameCache: coverage survives the round trip` (lines ~159, ~164, ~173)
- `FrameCache: writeback is batched, not once per frame` (line ~201)

**Delete `FrameCache::read_frame_range agrees with read_pixel exactly`
outright.** Do not convert it — `gather_pixel` is built on `read_frame_range`,
so a converted version would compare the function to itself and assert
nothing. It was scaffolding: its job was to prove the new API against the old
one while both were on the same layout, and that job finished the moment the
layout changed. What it was guarding is now covered end to end by the
characterization test in Step 8.

Keep `FrameCache::read_frame_range refuses ranges it cannot serve` — it never
referenced `read_pixel` and it is the only thing asserting the total-refusal
contract.

- [ ] **Step 6: Update the stale comment in the writeback test**

The case at line ~177 opens with "The layout is pixel-major, so ONE frame's
writes are strided across the whole mapping". That is now false. Replace that
test's comment block with:

```cpp
    // Writeback is bounded but not per-frame-per-mapping. Under the old
    // pixel-major layout one frame's writes were strided across the whole
    // mapping, so msync()ing all of it after every frame forced the entire
    // cache file back to disk for the 66 MB that frame actually held --
    // 1704 MB per frame measured on a 156-frame 24 MP run, 26x amplification.
    // Frame-major writes are sequential, so the sync can be narrowed instead
    // of skipped; see the following task.
```

Leave the assertions alone in this task — Task 5 rewrites them.

- [ ] **Step 7: Run the FrameCache tests**

```bash
cmake --build build -j$(nproc) --target test_frame_cache && ./build/test/test_frame_cache
```

Expected: **All tests passed**, including the new layout case. Every migrated
case asserts the same values as before — the encoding did not change, only
where the bytes sit.

- [ ] **Step 8: Run the characterization test — the real gate**

```bash
cmake --build build -j$(nproc) --target test_shadow_buffers && ./build/test/test_shadow_buffers
```

Expected: **All tests passed**, unedited since Task 1. This is the assertion
that Phase B sees identical data across the layout change.

- [ ] **Step 9: Run everything**

```bash
cd build && ctest --output-on-failure
cd .. && ./build/test/integration/test_lrgb_mono "[integration]" \
      && ./build/test/integration/test_phase_a_router "[integration]" \
      && ./build/test/integration/test_phase_b_qsolve "[integration]"
```

Expected: all green.

- [ ] **Step 10: Commit**

```bash
git add src/lib/stacker/include/nukex/stacker/frame_cache.hpp \
        src/lib/stacker/src/frame_cache.cpp \
        test/unit/stacker/test_frame_cache.cpp
git commit -m "perf(stacker): store the frame cache frame-major

offset(x,y,ch,f) becomes f*(W*H*nc) + (y*W+x)*nc + ch. One frame's pixels are
now consecutive, so Phase A writes 66 MB sequentially instead of dirtying
every page of a 10.4 GB mapping -- the 26x write amplification measured from
/proc/diskstats on a 156-frame 24 MP run, and the reason the per-frame cost
climbed from 3.85 s to 13.4 s as the cache filled.

read_pixel is deleted rather than kept as a slow fallback: gathering one pixel
across all frames is exactly what this layout is worst at, and leaving the
method would invite an accidental O(n^2) caller. Phase B has read by frame-row
since the previous commit.

The layout is asserted directly through a public element_offset() rather than
inferred from read-back values -- the storage order is the point of this
class, and it should not be possible to change it silently.

No output change: the characterization test from two commits ago passes
unedited."
```

---

### Task 5: Narrow the writeback to the frame that was just written

The 8-frame writeback batching exists only because a per-frame `msync` cost
the whole mapping. Frame-major makes one frame's dirty pages a contiguous
range, so writeback can go back to per-frame while touching 66 MB instead of
10.4 GB — which is a tighter dirty-page bound than the current code has, not
a looser one. The 2026-09-05 OOM this guards against was PixInsight holding
5.2 GB of dirty page cache beside a 14.8 GB cube.

**Files:**
- Modify: `src/lib/stacker/include/nukex/stacker/frame_cache.hpp`
- Modify: `src/lib/stacker/src/frame_cache.cpp`
- Test: `test/unit/stacker/test_frame_cache.cpp`

**Interfaces:**
- Consumes: `element_offset` from Task 4.
- Produces: no public API change. `sync_count()` keeps its meaning (number of
  writebacks scheduled) but now counts one per frame.

- [ ] **Step 1: Write the failing test**

Replace the `FrameCache: writeback is batched, not once per frame` case
wholesale with:

```cpp
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
```

- [ ] **Step 2: Run it to see it fail**

```bash
cmake --build build -j$(nproc) --target test_frame_cache && ./build/test/test_frame_cache "[frame_cache]"
```

Expected: FAIL at `REQUIRE(cache.sync_count() == 32)` with
`sync_count() == 4` (32 frames / `kSyncEveryNFrames` of 8).

- [ ] **Step 3: Add the ranged sync**

In `frame_cache.hpp`, delete `kSyncEveryNFrames` and declare, in the private
section beside `cleanup()`:

```cpp
    /// Schedule writeback of one byte range of the mapping.
    ///
    /// msync needs a page-aligned address, so the range is widened outward to
    /// page boundaries -- writing back a few neighbouring pages is free
    /// compared with the alternative of writing back all of them.
    void sync_range(std::size_t byte_begin, std::size_t byte_end);
```

and update the `sync_count()` doc comment:

```cpp
    /// How many times writeback has been scheduled -- one per cached frame
    /// plus any explicit flush(). Diagnostic: with a frame-major layout each
    /// sync covers only that frame's own bytes, so this number multiplied by
    /// one frame's size is roughly the volume written.
    int sync_count() const { return sync_count_; }
```

In `frame_cache.cpp`, add `#include <unistd.h>` if it is not already present
(it is, for `close`/`unlink`), and add:

```cpp
void FrameCache::sync_range(std::size_t byte_begin, std::size_t byte_end) {
    if (!mapped_ || byte_end <= byte_begin) return;
    const std::size_t page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const std::size_t begin = byte_begin & ~(page - 1);
    std::size_t end = (byte_end + page - 1) & ~(page - 1);
    if (end > mapped_size_) end = mapped_size_;
    if (end <= begin) return;
    // MS_ASYNC only schedules it, so this never stalls the caching loop.
    msync(reinterpret_cast<char*>(mapped_) + begin, end - begin, MS_ASYNC);
}
```

- [ ] **Step 4: Call it from `write_frame`**

Replace the `if ((frame_index + 1) % kSyncEveryNFrames == 0) { ... }` block
and its comment with:

```cpp
    // Schedule writeback of THIS frame's bytes, and nothing else.
    //
    // Dirty pages have to be bounded: on 2026-09-05 a 33-frame 24 MP OSC
    // cache held 5.2 GB of dirty page cache beside a 14.8 GB voxel cube and
    // PixInsight was OOM-killed mid-cache. Under the old pixel-major layout
    // bounding them per frame was ruinous -- one frame's writes were strided
    // across the whole mapping, so an msync of it pushed the entire cache
    // file back to disk for the 66 MB that frame actually held. Frame-major
    // makes the frame's dirty pages contiguous, so the bound is per-frame
    // again and costs one frame's worth of I/O.
    const std::size_t frame_elems =
        static_cast<std::size_t>(width_) * height_ * n_channels_;
    const std::size_t values_begin =
        frame_elems * static_cast<std::size_t>(frame_index) * sizeof(uint16_t);
    sync_range(values_begin, values_begin + frame_elems * sizeof(uint16_t));

    // The coverage bitplane is a second contiguous range, one bit per entry,
    // indexed identically to the values.
    const std::size_t plane_base =
        frame_elems * static_cast<std::size_t>(max_frames_) * sizeof(uint16_t);
    sync_range(plane_base + (frame_elems * static_cast<std::size_t>(frame_index)) / 8,
               plane_base + (frame_elems * static_cast<std::size_t>(frame_index + 1) + 7) / 8);
    ++sync_count_;
```

One `++sync_count_` for the pair, so the counter stays "writebacks per frame".

- [ ] **Step 5: Run the tests**

```bash
cmake --build build -j$(nproc) --target test_frame_cache && ./build/test/test_frame_cache
```

Expected: **All tests passed**, including `sync_count() == 32`.

- [ ] **Step 6: Run everything**

```bash
cd build && ctest --output-on-failure
cd .. && ./build/test/integration/test_lrgb_mono "[integration]" \
      && ./build/test/integration/test_phase_a_router "[integration]" \
      && ./build/test/integration/test_phase_b_qsolve "[integration]"
```

Expected: all green.

- [ ] **Step 7: Commit**

```bash
git add src/lib/stacker/include/nukex/stacker/frame_cache.hpp \
        src/lib/stacker/src/frame_cache.cpp \
        test/unit/stacker/test_frame_cache.cpp
git commit -m "perf(stacker): sync the frame that was written, not the mapping

Syncing every 8th frame was a workaround for the pixel-major layout, where one
frame's writes were strided across the whole mapping and an msync of it pushed
the entire cache file to disk for the 66 MB that frame actually held.

Frame-major makes those pages contiguous, so writeback goes back to per-frame
and covers one frame's own bytes plus its slice of the coverage plane. That is
a tighter dirty-page bound than the batching it replaces -- which is the point:
on 2026-09-05 a 33-frame OSC cache held 5.2 GB of dirty pages beside a 14.8 GB
cube and PixInsight was OOM-killed mid-cache.

msync needs a page-aligned address, so the range is widened outward to page
boundaries."
```

---

### Task 6: Prove it on the real corpora and record the measurement

Nothing is done until the four-corpus E2E comes back bit-identical and the
speedup is a measured number rather than a projection. Budget several hours:
the manifest allows 3600 s per case.

**Files:**
- Modify: `docs/superpowers/specs/2026-09-07-phase-a-performance-design.md`
  (section 3 gains a `3c`, section 5 ticks item 3)
- No source changes expected. If the E2E moves a hash, STOP — that is a
  defect in Tasks 3-5, not a golden to re-cut.

**Interfaces:**
- Consumes: everything from Tasks 2-5.
- Produces: the measured numbers that the next piece of work (parallel Phase A,
  spec 4b) is sized against.

- [ ] **Step 1: Build the module cleanly**

```bash
cmake -S . -B build && cmake --build build -j$(nproc) 2>&1 | tail -20
ls -la build/src/module/NukeX-pxm.so
```

Expected: the module builds with no warnings introduced by this work.

- [ ] **Step 2: Capture the write amplification, before and after**

Run one corpus and sample `/proc/diskstats` across Phase A. On this box the
NVMe is the device backing the cache directory; find it with `df` then read
the sectors-written column (field 7 of `/proc/diskstats`, 512-byte sectors).

```bash
# In one shell, note the device backing the cache dir:
df --output=source "$(dirname "${TMPDIR:-/tmp}")"
# Sample before and after Phase A of a single E2E case:
grep -E ' nvme0n1 ' /proc/diskstats | awk '{print $10 * 512 / 1048576 " MB"}'
```

Record MB written per cached frame. The spec's baseline is **1,704 MB per
frame** for 66 MB of data. Expect it to land near 66-80 MB per frame.

If you cannot get a clean device-level reading, the per-frame timings in the
E2E log are the fallback signal — the spec's baseline is 3.85 s at frame 1
rising to 13.4 s at frame 140, and the degradation is what should disappear.

- [ ] **Step 3: Run the full E2E in VERIFY mode**

```bash
make -C build e2e 2>&1 | tee /tmp/e2e-frame-major.log
```

Do **not** pass `regen`. Do **not** set `NUKEX_E2E_REGEN`.

Expected: PASS on all four cases — `lrgb_mono_ngc7635`,
`bayer_rgb_m27_2023`, `bayer_nb_hao3_m16`, `mono_lrgb_m27_2025` — with
`regen=False, checked=True, match=True` on every one, including all three
dropdown sweeps per case.

**If any hash moves:** the layout change is not bit-identical and something in
Tasks 3-5 is wrong. Report which case and which artefact
(`stacked` / `noise` / `composed` / `stretched` / a sweep) moved and stop.
`stacked` moving points at the cache read path; `stretched` alone moving would
be impossible from this work and points at contamination from another change.

- [ ] **Step 4: Read the Phase A timings out of the log**

```bash
grep -E "Phase A|Phase B|frame [0-9]+" /tmp/e2e-frame-major.log | tail -40
```

Record: Phase A total, Phase B total, first-frame and last-frame per-frame
cost. The baseline to beat, from the spec on a 156-frame M63 batch: Phase A
1133 s of a 1689 s run, with per-frame cost degrading 3.85 s → 13.4 s.

- [ ] **Step 5: Write the measurement into the spec**

In `docs/superpowers/specs/2026-09-07-phase-a-performance-design.md`, add a
subsection after `### 3b. Bounded GPU staging budget (implemented)`:

```markdown
### 3c. Frame-major layout (implemented)

`offset(x, y, ch, f) = f*(W*H*n_ch) + (y*W+x)*n_ch + ch`. Phase B reads by
frame-row (`FrameCache::read_frame_range`) instead of gathering one pixel
across all frames; `read_pixel` is gone. Writeback narrowed to the frame just
written, replacing the 8-frame batching that the old layout forced.

Measured: [MB per frame written, was 1704] / [Phase A total, was 1133 s] /
[per-frame first→last, was 3.85 s → 13.4 s].

E2E: all four corpora bit-identical, verify mode, no golden re-cut.
```

Fill in the bracketed values with what Steps 2 and 4 produced. Then in
section 5, change item 3 from `**4a frame-major layout** — ...` to
`~~4a frame-major layout~~ (done, [Nx])`.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-09-07-phase-a-performance-design.md
git commit -m "docs(spec): record the frame-major layout result

E2E verify PASS on all four corpora with no hash moved, which is the property
this change was gated on: it is a storage-order change and must not touch a
single output pixel.

Numbers in section 3c."
```

- [ ] **Step 7: Report, do not release**

This is a performance change with no user-visible behaviour, so it does not
need its own release. Report to the user:

- the measured write amplification and Phase A time, against the baselines;
- E2E verify result, per case;
- that no golden was regenerated.

Then ask whether to fold it into the next release or ship it on its own. The
release workflow (version bump, sign, package, publish) is a separate,
user-gated step — see the PixInsight Release Workflow in CLAUDE.md and
`feedback_never_sudo_into_pixinsight`.

---

## Notes for whoever executes this

**The single-variable discipline is the whole design of this plan.** Task 3
changes how Phase B reads while the layout is untouched; Task 4 changes the
layout while the read path is untouched. If the E2E moves, the task boundary
tells you which half caused it. Collapsing them into one commit throws that
away, and this repo has needed it before — the v5.0.1.1 mono golden moved by
47 edge pixels and only a single-variable revert established why.

**A moved golden is a bug, never a re-cut.** Two of the four goldens are
non-frozen and one is a deliberately frozen floor, but that distinction does
not apply here: this change is storage order. It cannot legitimately move any
output pixel.

**`/tmp` is a 16 GB tmpfs on this box.** The unit tests write tiny caches
there and are fine. Real E2E runs must not put the cache on tmpfs — a cache
there OOM'd the machine once already (`project_e2e_environment_facts`).

**Do not run E2E corpora concurrently.** Three concurrent stacks OOM'd the
30 GB box and killed all three jobs.
