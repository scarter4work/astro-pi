### Task 1: Star detection reads green, not channel 0

`StarDetector::detect` hardcodes channel `0` in nine places. On a debayered OSC frame that is red — the channel with the largest lateral-colour error and, through a quad-band filter, often the weakest signal. Green has two of every four photosites and is the right choice for both star detection and the registration reference.

This task changes which channel is detected on. That alone moves every Bayer golden, which is expected and handled in Task 7.

**Files:**
- Modify: `src/lib/alignment/include/nukex/alignment/star_detector.hpp`
- Modify: `src/lib/alignment/src/star_detector.cpp`
- Test: `test/unit/alignment/test_star_detector.cpp` (append)

**Interfaces:**
- Consumes: nothing.
- Produces: `int nukex::default_reference_channel(int n_channels)` declared in `star_detector.hpp`; `StarDetector::Config::channel` (an `int`, default `-1` meaning auto).

- [ ] **Step 1: Write the failing test**

Append to `test/unit/alignment/test_star_detector.cpp`:

```cpp
// --- green as the detection channel -------------------------------------

TEST_CASE("default_reference_channel: green for colour, channel 0 for mono",
          "[star_detector]") {
    REQUIRE(nukex::default_reference_channel(1) == 0);
    REQUIRE(nukex::default_reference_channel(2) == 0);
    REQUIRE(nukex::default_reference_channel(3) == 1);
    REQUIRE(nukex::default_reference_channel(4) == 1);
}

TEST_CASE("StarDetector detects on green, not on channel 0",
          "[star_detector]") {
    // Red carries one star, green carries three. Detecting on channel 0 finds
    // one; detecting on green finds three. This is the whole point: on an OSC
    // frame channel 0 is red, and red is the channel we least want to trust.
    nukex::Image img(120, 120, 3);
    img.fill(0.01f);

    auto blob = [&](int ch, float cx, float cy) {
        for (int dy = -4; dy <= 4; dy++)
            for (int dx = -4; dx <= 4; dx++) {
                float r2 = float(dx * dx + dy * dy);
                img.at(int(cx) + dx, int(cy) + dy, ch) += 0.6f * std::exp(-r2 / 3.0f);
            }
    };

    blob(0, 30, 30);                 // red: one star
    blob(1, 30, 30);
    blob(1, 70, 40);                 // green: three stars
    blob(1, 50, 85);

    nukex::StarDetector::Config cfg;
    cfg.snr_multiplier = 3.0f;

    auto cat = nukex::StarDetector::detect(img, cfg);
    REQUIRE(cat.size() == 3);

    // And the channel is overridable, which is how the ladder tests in
    // Task 3 pin a specific plane.
    cfg.channel = 0;
    REQUIRE(nukex::StarDetector::detect(img, cfg).size() == 1);
}
```

If `test_star_detector.cpp` does not already include `<cmath>`, add it.

- [ ] **Step 2: Run the test and watch it fail**

```bash
cmake --build build -j$(nproc) --target test_star_detector 2>&1 | tail -20
```

Expected: a **compile** failure — `default_reference_channel` is not declared and `Config` has no member `channel`. That is the correct first failure. Do not proceed until you have seen it.

- [ ] **Step 3: Declare the config field and the helper**

In `src/lib/alignment/include/nukex/alignment/star_detector.hpp`, add to `StarDetector::Config`, after `saturation_reject_fraction`:

```cpp
        /// Which channel to detect stars on. -1 means auto: green (channel 1)
        /// for any image with 3 or more channels, channel 0 otherwise.
        ///
        /// Green is the right default for a colour frame. It has two of every
        /// four photosites on an RGGB sensor, so its centroids are the least
        /// noisy available, and through a multi-band filter it is not the
        /// starved channel. Detecting on channel 0 -- red, after debayer --
        /// registers frames using the channel that carries the lateral-colour
        /// error, which is exactly backwards.
        int channel = -1;
```

Then, before `class StarDetector`, in namespace `nukex`:

```cpp
/// The channel star detection and channel registration both use as their
/// reference: green for a colour image, the only channel for a mono one.
///
/// Free rather than a member because channel registration needs the same
/// answer and must not depend on StarDetector to get it.
inline int default_reference_channel(int n_channels) {
    return n_channels >= 3 ? 1 : 0;
}
```

- [ ] **Step 4: Thread the channel through the implementation**

In `src/lib/alignment/src/star_detector.cpp`, change the four private helpers to take a channel, and replace every literal `0` in a `.at(x, y, 0)` call inside them with that parameter.

Header declarations become:

```cpp
    static std::pair<float, float> compute_background_noise(const Image& image, int ch);

    static std::vector<std::tuple<int, int, float>> find_local_maxima(
        const Image& image, float threshold, int exclusion_radius, int ch);

    static std::pair<float, float> refine_centroid(
        const Image& image, int x, int y, int ch);

    static float compute_flux(const Image& image, float cx, float cy,
                              float background, int ch, int aperture_radius = 5);
```

In `detect()`, resolve the channel once at the top, immediately after the empty/size guard:

```cpp
    const int ch = (config.channel >= 0 && config.channel < image.n_channels())
                 ? config.channel
                 : default_reference_channel(image.n_channels());
```

Then pass `ch` at each of the four call sites, and change the one inline `image.at(px, py, 0)` in the FWHM second-moment block inside `detect()` to `image.at(px, py, ch)`.

Leave `saturation_fraction` on channel 0. It answers "is this frame clipped", which is a property of the frame rather than of a colour, and moving it would change which frames get rejected as blown out for reasons unrelated to this work. Add a comment saying so:

```cpp
/// Deliberately measured on channel 0 rather than the detection channel:
/// this answers "is this frame clipped", which is a property of the exposure,
/// not of a colour. Changing it would move the blown-out cut for reasons that
/// have nothing to do with channel registration.
```

- [ ] **Step 5: Run the test and watch it pass**

```bash
cmake --build build -j$(nproc) --target test_star_detector && ./build/test/test_star_detector
```

Expected: PASS, all cases.

- [ ] **Step 6: Run the whole suite — other alignment tests use synthetic mono images and must be unaffected**

```bash
ctest --test-dir build --output-on-failure
```

Expected: all green. A failure in `test_frame_aligner` or `test_homography` here means a synthetic fixture was relying on channel 0 of a colour image; fix the fixture, not the production code.

- [ ] **Step 7: Commit**

```bash
git add src/lib/alignment/include/nukex/alignment/star_detector.hpp \
        src/lib/alignment/src/star_detector.cpp \
        test/unit/alignment/test_star_detector.cpp
git commit -m "feat(alignment): detect stars on green, not on channel 0

On a debayered OSC frame channel 0 is red -- the channel carrying the
lateral-colour error and, through a quad-band filter, often the weakest
signal. Green has two of every four photosites, so its centroids are the
best available. Mono is unchanged: default_reference_channel returns 0
below three channels."
```

---

