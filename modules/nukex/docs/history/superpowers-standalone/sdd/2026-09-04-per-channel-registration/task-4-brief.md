### Task 4: Compose the transform into the warp

The correction must cost nothing extra. `HomographyComputer::warp` already back-maps every output pixel to a source coordinate and already loops over channels. Applying the channel transform to that back-mapped coordinate, before the bounds check, folds the correction into a resample that was happening anyway.

The composition, stated exactly: `H_inv` maps an output pixel to where the **reference channel** sees it in the source frame. `A_c` maps a reference-channel position to where **channel c** sees it in the same frame. So the sample position for channel c is `A_c(H_inv(X, Y))`. The spec writes this as warping with `H · A_c`; that is the same operation seen from the forward direction.

**Files:**
- Modify: `src/lib/alignment/include/nukex/alignment/homography.hpp`
- Modify: `src/lib/alignment/src/homography.cpp`
- Test: `test/unit/alignment/test_homography.cpp` (append)

**Interfaces:**
- Consumes: `ChannelTransforms`, `ChannelTransform::apply` from Task 2.
- Produces: `static Image HomographyComputer::warp(const Image& source, const HomographyMatrix& H, int output_width, int output_height, const ChannelTransforms& channels)`. The existing four-argument overload keeps its exact signature and behaviour.

- [ ] **Step 1: Write the failing test**

Append to `test/unit/alignment/test_homography.cpp`:

```cpp
// --- channel-aware warp --------------------------------------------------

#include "nukex/alignment/channel_registration.hpp"

TEST_CASE("warp with channel transforms brings a displaced channel into "
          "register", "[homography]") {
    // Red drawn 1.5 px right of green. A channel transform of exactly that
    // must pull it back on top.
    nukex::Image src(200, 200, 3);
    src.fill(0.0f);

    auto blob = [&](int ch, double cx, double cy) {
        for (int dy = -6; dy <= 6; dy++)
            for (int dx = -6; dx <= 6; dx++) {
                int px = int(std::lround(cx)) + dx;
                int py = int(std::lround(cy)) + dy;
                if (px < 0 || px >= 200 || py < 0 || py >= 200) continue;
                double ex = px - cx, ey = py - cy;
                src.at(px, py, ch) += float(0.5 * std::exp(-(ex*ex + ey*ey) / 5.12));
            }
    };
    blob(1, 100.0, 100.0);   // green
    blob(0, 101.5, 100.0);   // red, displaced

    nukex::ChannelTransforms ct;
    ct.cx = 99.5; ct.cy = 99.5;
    ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[0].s  = 1.0;
    ct.per_channel[0].tx = 1.5;         // where red images a green position
    ct.per_channel[0].fit = nukex::ChannelTransform::Fit::TranslationOnly;

    nukex::Image out = nukex::HomographyComputer::warp(
        src, nukex::HomographyMatrix::identity(), 200, 200, ct);

    // Centre of mass of each channel in a box around the green position.
    auto com_x = [&](const nukex::Image& im, int ch) {
        double w = 0, wx = 0;
        for (int y = 90; y < 110; y++)
            for (int x = 90; x < 112; x++) {
                double v = im.at(x, y, ch);
                if (v <= 0) continue;
                w += v; wx += v * x;
            }
        return wx / w;
    };

    // Before: red sits 1.5 px away. After: within a twentieth of a pixel.
    REQUIRE(std::abs(com_x(src, 0) - com_x(src, 1)) > 1.4);
    REQUIRE(std::abs(com_x(out, 0) - com_x(out, 1)) < 0.05);

    // Green must be untouched. It is the reference; resampling it would blur
    // it for nothing, and the acceptance criterion checks its FWHM.
    for (int y = 0; y < 200; y++)
        for (int x = 0; x < 200; x++)
            REQUIRE(out.at(x, y, 1) == Catch::Approx(src.at(x, y, 1)));
}

TEST_CASE("warp with empty channel transforms matches the old warp exactly",
          "[homography]") {
    nukex::Image src(64, 64, 3);
    for (int c = 0; c < 3; c++)
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++)
                src.at(x, y, c) = float((x * 7 + y * 13 + c * 29) % 251) / 251.0f;

    nukex::HomographyMatrix H = nukex::HomographyMatrix::identity();
    H(0, 2) = 2.5f;
    H(1, 2) = -1.25f;

    nukex::Image a = nukex::HomographyComputer::warp(src, H, 64, 64);
    nukex::Image b = nukex::HomographyComputer::warp(src, H, 64, 64,
                                                    nukex::ChannelTransforms{});

    REQUIRE(a.data_size() == b.data_size());
    for (size_t i = 0; i < a.data_size(); i++)
        REQUIRE(a.data()[i] == b.data()[i]);
}
```

- [ ] **Step 2: Run and watch it fail**

```bash
cmake --build build -j$(nproc) --target test_homography 2>&1 | tail -20
```

Expected: compile failure — no five-argument `warp`.

- [ ] **Step 3: Declare the overload**

In `src/lib/alignment/include/nukex/alignment/homography.hpp`, add the include:

```cpp
#include "nukex/alignment/channel_registration.hpp"
```

and, immediately after the existing `warp` declaration:

```cpp
    /// Warp, additionally registering each channel to the reference channel.
    ///
    /// H_inv maps an output pixel to where the REFERENCE channel sees it in
    /// the source. A_c then maps that to where channel c sees it. So the
    /// sample position for channel c is A_c(H_inv(x, y)) -- one resample per
    /// channel, exactly as the plain warp does, with the correction folded in
    /// rather than applied as a second pass.
    ///
    /// An empty `channels`, or an identity entry within it, takes the same
    /// path as the four-argument overload for that channel.
    static Image warp(const Image& source, const HomographyMatrix& H,
                      int output_width, int output_height,
                      const ChannelTransforms& channels);
```

- [ ] **Step 4: Implement it**

In `src/lib/alignment/src/homography.cpp`, rename the existing definition to take the extra argument and make the old signature delegate:

```cpp
Image HomographyComputer::warp(const Image& source, const HomographyMatrix& H,
                               int output_width, int output_height) {
    return warp(source, H, output_width, output_height, ChannelTransforms{});
}

Image HomographyComputer::warp(const Image& source, const HomographyMatrix& H,
                               int output_width, int output_height,
                               const ChannelTransforms& channels) {
```

Inside the channel loop, before the `for (int y ...)`, hoist the per-channel decision out of the pixel loop:

```cpp
    for (int ch = 0; ch < source.n_channels(); ch++) {
        const bool has_ct = ch < static_cast<int>(channels.per_channel.size());
        const ChannelTransform ct =
            has_ct ? channels.per_channel[ch] : ChannelTransform{};
        const bool apply_ct = has_ct && !ct.is_identity();
```

Then, in the pixel body, apply it **after** computing `sx`, `sy` from `H_inv` and **before** the finite and bounds checks. The order matters: a coordinate can be inside the frame before the channel shift and outside after, and sampling it then would read out of bounds.

```cpp
                float sx = (H_inv(0, 0) * x + H_inv(0, 1) * y + H_inv(0, 2)) / w;
                float sy = (H_inv(1, 0) * x + H_inv(1, 1) * y + H_inv(1, 2)) / w;

                if (apply_ct) ct.apply(channels.cx, channels.cy, sx, sy);

                // Bilinear interpolation
                if (!std::isfinite(sx) || !std::isfinite(sy)) continue;
                if (sx < 0 || sx >= sw - 1 || sy < 0 || sy >= sh - 1) continue;
```

Leave the rest of the loop untouched.

- [ ] **Step 5: Run and watch it pass**

```bash
cmake --build build -j$(nproc) --target test_homography && ./build/test/test_homography
```

Expected: PASS, including the bit-identical check against the old warp.

- [ ] **Step 6: Commit**

```bash
git add src/lib/alignment/include/nukex/alignment/homography.hpp \
        src/lib/alignment/src/homography.cpp \
        test/unit/alignment/test_homography.cpp
git commit -m "feat(alignment): channel-aware warp overload

Applies the per-channel transform to the back-mapped source coordinate,
before the bounds check, so the correction rides along with the resample
that was already happening. One resample per channel, as before.

The four-argument warp delegates with empty transforms and is proven
bit-identical to its old self."
```

---

