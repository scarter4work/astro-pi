### Task 5: Wire it into FrameAligner

The measurement happens on the un-warped frame, using the catalog `align()` has already computed. Three paths through `align()` produce an image and all three need the correction, including the two that do not warp today.

That last part is the subtle one. The reference frame is currently `frame.clone()` with an identity homography — but its own channels disagree with each other, and every other frame is being registered to it. If the reference is not corrected, the whole stack inherits its channel error. The same applies to a frame whose alignment failed: it is still stacked, with a weight penalty, so it still needs its channels put right.

**Files:**
- Modify: `src/lib/alignment/include/nukex/alignment/frame_aligner.hpp`
- Modify: `src/lib/alignment/src/frame_aligner.cpp`
- Test: `test/unit/alignment/test_frame_aligner.cpp` (append)

**Interfaces:**
- Consumes: `measure_channel_transforms`, `ChannelTransforms`, `ChannelRegistrationConfig`, the five-argument `warp`, `default_reference_channel`.
- Produces: `FrameAligner::AlignedFrame::channels` (a `ChannelTransforms`); `FrameAligner::Config::channel_config` (a `ChannelRegistrationConfig`); `FrameAligner::Config::register_channels` (a `bool`, default `true`).

- [ ] **Step 1: Write the failing test**

Append to `test/unit/alignment/test_frame_aligner.cpp`:

```cpp
// --- per-channel registration through the aligner ------------------------

#include "nukex/alignment/channel_registration.hpp"

namespace {

// A 3-channel frame with a star grid; red displaced by a pure translation.
nukex::Image make_colour_frame(int w, int h, double red_dx, double red_dy,
                               double jitter_x = 0.0, double jitter_y = 0.0) {
    nukex::Image img(w, h, 3);
    img.fill(0.002f);
    auto blob = [&](int ch, double cx, double cy) {
        for (int dy = -6; dy <= 6; dy++)
            for (int dx = -6; dx <= 6; dx++) {
                int px = int(std::lround(cx)) + dx;
                int py = int(std::lround(cy)) + dy;
                if (px < 0 || px >= w || py < 0 || py >= h) continue;
                double ex = px - cx, ey = py - cy;
                img.at(px, py, ch) += float(0.5 * std::exp(-(ex*ex + ey*ey) / 5.12));
            }
    };
    for (int j = 0; j < 5; j++)
        for (int i = 0; i < 5; i++) {
            double x = 40.0 + 0.37 + i * (w - 80.0) / 4.0 + jitter_x;
            double y = 40.0 + 0.61 + j * (h - 80.0) / 4.0 + jitter_y;
            blob(1, x, y);
            blob(2, x, y);
            blob(0, x + red_dx, y + red_dy);
        }
    return img;
}

double channel_offset_x(const nukex::Image& im, int ch_a, int ch_b,
                        int x0, int x1, int y0, int y1) {
    auto com = [&](int ch) {
        double w = 0, wx = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                double v = im.at(x, y, ch) - 0.002;
                if (v <= 0) continue;
                w += v; wx += v * x;
            }
        return wx / w;
    };
    return com(ch_a) - com(ch_b);
}

} // namespace

TEST_CASE("FrameAligner registers channels on the reference frame itself",
          "[frame_aligner]") {
    // The reference frame is not warped for alignment -- H is identity by
    // construction. Its channels still have to be put right, or every other
    // frame inherits its colour error through the reference.
    nukex::Image ref = make_colour_frame(400, 400, 1.2, 0.0);

    nukex::FrameAligner aligner;
    aligner.set_reference(ref, 0);
    auto out = aligner.align(ref, 0);

    REQUIRE_FALSE(out.channels.empty());
    REQUIRE(out.channels.reference_channel == 1);

    CHECK(std::abs(channel_offset_x(ref, 0, 1, 30, 90, 30, 90)) > 1.0);
    CHECK(std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) < 0.06);
}

TEST_CASE("FrameAligner registers channels on a warped frame",
          "[frame_aligner]") {
    nukex::Image ref   = make_colour_frame(400, 400, 1.2, 0.0);
    nukex::Image moved = make_colour_frame(400, 400, 1.2, 0.0, 3.0, 2.0);

    nukex::FrameAligner aligner;
    aligner.set_reference(ref, 0);
    (void)aligner.align(ref, 0);
    auto out = aligner.align(moved, 1);

    REQUIRE_FALSE(out.alignment.alignment_failed);
    CHECK(std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) < 0.06);
}

TEST_CASE("a mono frame produces no channel transforms and is untouched",
          "[frame_aligner]") {
    nukex::Image mono(300, 300, 1);
    mono.fill(0.002f);
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            double cx = 50.0 + 0.3 + i * 66.0, cy = 50.0 + 0.7 + j * 66.0;
            for (int dy = -6; dy <= 6; dy++)
                for (int dx = -6; dx <= 6; dx++) {
                    int px = int(std::lround(cx)) + dx, py = int(std::lround(cy)) + dy;
                    if (px < 0 || px >= 300 || py < 0 || py >= 300) continue;
                    double ex = px - cx, ey = py - cy;
                    mono.at(px, py, 0) += float(0.5 * std::exp(-(ex*ex+ey*ey)/5.12));
                }
        }

    nukex::FrameAligner aligner;
    aligner.set_reference(mono, 0);
    auto out = aligner.align(mono, 0);

    REQUIRE(out.channels.empty());
    for (int y = 0; y < 300; y++)
        for (int x = 0; x < 300; x++)
            REQUIRE(out.image.at(x, y, 0) == mono.at(x, y, 0));
}

TEST_CASE("a frame whose channels already agree is not resampled for it",
          "[frame_aligner]") {
    // All three channels drawn at the same positions. The near-identity skip
    // must take the plain path, leaving the reference frame bit-identical.
    nukex::Image ref = make_colour_frame(400, 400, 0.0, 0.0);

    nukex::FrameAligner aligner;
    aligner.set_reference(ref, 0);
    auto out = aligner.align(ref, 0);

    for (int c = 0; c < 3; c++)
        for (int y = 0; y < 400; y++)
            for (int x = 0; x < 400; x++)
                REQUIRE(out.image.at(x, y, c) == ref.at(x, y, c));
}

TEST_CASE("channel registration can be switched off",
          "[frame_aligner]") {
    nukex::Image ref = make_colour_frame(400, 400, 1.2, 0.0);

    nukex::FrameAligner::Config cfg;
    cfg.register_channels = false;
    nukex::FrameAligner aligner(cfg);
    aligner.set_reference(ref, 0);
    auto out = aligner.align(ref, 0);

    REQUIRE(out.channels.empty());
    CHECK(std::abs(channel_offset_x(out.image, 0, 1, 30, 90, 30, 90)) > 1.0);
}
```

If `test_frame_aligner.cpp` lacks `<cmath>`, add it.

- [ ] **Step 2: Run and watch it fail**

```bash
cmake --build build -j$(nproc) --target test_frame_aligner 2>&1 | tail -20
```

Expected: compile failure — `AlignedFrame` has no member `channels`, `Config` has no `register_channels`.

- [ ] **Step 3: Extend the header**

In `src/lib/alignment/include/nukex/alignment/frame_aligner.hpp`:

Add the include:
```cpp
#include "nukex/alignment/channel_registration.hpp"
```

Add to `Config`:
```cpp
        ChannelRegistrationConfig channel_config;

        /// Register the colour channels to each other within each frame.
        ///
        /// On by default and with no user-facing threshold, deliberately:
        /// almost nobody knows they have lateral chromatic aberration, so an
        /// opt-in would not reach the people it helps. The cost is bounded by
        /// the near-identity skip -- a rig with no colour error pays nothing.
        /// The flag exists so tests can isolate the old behaviour.
        bool register_channels = true;
```

Add to `AlignedFrame`, after `stars`:
```cpp
        /// Per-channel transforms measured on this frame, before warping.
        /// Empty for a mono frame, when registration is off, or when nothing
        /// could be measured.
        ChannelTransforms channels;
```

- [ ] **Step 4: Implement in `align()`**

In `src/lib/alignment/src/frame_aligner.cpp`, rewrite `align()`:

```cpp
FrameAligner::AlignedFrame FrameAligner::align(const Image& frame, int frame_index) {
    AlignedFrame result;
    result.frame_index = frame_index;

    // Detect stars
    result.stars = StarDetector::detect(frame, config_.star_config);

    if (!has_ref_) {
        // No reference was set: fall back to the first frame to arrive.
        adopt_reference(frame, result.stars, frame_index);
    }

    // Measure the channel disagreement on the UNWARPED frame, using the stars
    // we already have. Lateral colour is a property of this exposure through
    // this optic at this altitude; measuring it after warping would mix it
    // with the frame-to-frame transform.
    if (config_.register_channels && frame.n_channels() >= 2
        && !result.stars.empty()) {
        result.channels = measure_channel_transforms(
            frame, result.stars,
            default_reference_channel(frame.n_channels()),
            config_.channel_config);
    }

    // The near-identity skip. A well-corrected rig should not pay for
    // interpolation it does not need, and this is what makes "always on"
    // affordable without exposing a threshold for users to argue about.
    // The radius is the frame's own corner, where the scale term is largest.
    const double corner_radius =
        std::hypot(frame.width() / 2.0, frame.height() / 2.0);
    const bool channels_matter =
        !result.channels.empty()
        && !result.channels.negligible(corner_radius,
                                       kNegligibleChannelShiftPx);
    if (!channels_matter) result.channels = ChannelTransforms{};

    if (frame_index == ref_index_) {
        // This IS the reference. Matching it against its own catalog would
        // only reintroduce fit noise into a transform that is exactly the
        // identity by construction.
        //
        // Its CHANNELS are a different matter. Every other frame registers to
        // this one, so if its own channels disagree the whole stack inherits
        // that. Warp it with the identity homography when there is a channel
        // correction to make, and clone it when there is not.
        result.alignment.H = HomographyMatrix::identity();
        result.alignment.match.success = true;
        result.alignment.match.n_inliers = result.stars.size();
        result.image = channels_matter
            ? HomographyComputer::warp(frame, result.alignment.H,
                                       frame.width(), frame.height(),
                                       result.channels)
            : frame.clone();
        return result;
    }

    // Match stars to reference using triangle similarity matching.
    auto matches = StarMatcher::match(result.stars, ref_catalog_,
                                       config_.match_config);

    // Compute homography
    result.alignment = HomographyComputer::compute(
        result.stars, ref_catalog_, matches, config_.homography_config);

    // Handle meridian flip
    if (result.alignment.is_meridian_flipped && !result.alignment.alignment_failed) {
        result.alignment.H = HomographyComputer::correct_meridian_flip(
            result.alignment.H, ref_width_, ref_height_);
    }

    if (!result.alignment.alignment_failed) {
        result.image = HomographyComputer::warp(
            frame, result.alignment.H, ref_width_, ref_height_,
            result.channels);
    } else if (channels_matter) {
        // Failed alignment: the frame is still stacked, with its weight
        // penalised, so its channels still have to be put right. The
        // homography is the identity because there isn't a usable one.
        result.image = HomographyComputer::warp(
            frame, HomographyMatrix::identity(),
            frame.width(), frame.height(), result.channels);
    } else {
        result.image = frame.clone();
    }

    return result;
}
```

Add at the top of the file, inside `namespace nukex`:

```cpp
namespace {
/// A channel correction smaller than this at the field corner is not worth a
/// resample: it is below the centroid noise floor measured on real data
/// (0.058 px between blue and green on the M3 set) by a wide margin.
constexpr double kNegligibleChannelShiftPx = 0.01;
}
```

and add `#include <cmath>` for `std::hypot`.

- [ ] **Step 5: Run and watch it pass**

```bash
cmake --build build -j$(nproc) --target test_frame_aligner && ./build/test/test_frame_aligner
```

Expected: all cases PASS, old and new.

- [ ] **Step 6: Run the whole suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: all green. `test_alignment_diag` cases are dot-tagged and will report as skipped.

- [ ] **Step 7: Commit**

```bash
git add src/lib/alignment/include/nukex/alignment/frame_aligner.hpp \
        src/lib/alignment/src/frame_aligner.cpp \
        test/unit/alignment/test_frame_aligner.cpp
git commit -m "feat(alignment): register channels through FrameAligner

Measured on the unwarped frame from the catalog align() already has, and
composed into the warp that was already going to run.

All three image paths get the correction, including the two that do not
warp today: the reference frame -- every other frame registers to it, so
its own channel error would propagate into the whole stack -- and a frame
whose alignment failed, which is still stacked with a weight penalty.

A frame whose channels agree to better than 0.01 px at the corner takes
the old path untouched, which is what makes always-on affordable."
```

---

