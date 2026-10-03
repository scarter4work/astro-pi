# Per-Channel Registration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Register a frame's colour channels to each other before stacking, so lateral chromatic aberration and atmospheric dispersion stop smearing stars across colour.

**Architecture:** A new component in `lib/alignment` centroids each channel at the star positions the aligner already found, fits scale-plus-translation per channel against green, and hands the result to the warp that was already going to run. One resample per channel, exactly as today. Green also becomes the star-detection channel; detecting on channel 0 means detecting on red for an OSC frame, which is the channel carrying the error.

**Tech Stack:** C++17, CMake, Catch2 v3 (amalgamated, `third_party/catch2`), Eigen 3 (already a private dep of `nukex4_alignment`). No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-04-per-channel-registration-design.md`

## Global Constraints

- **Library target:** `nukex4_alignment` (the `nukex4_` prefix is historical across the whole v5 tree — do not rename it).
- **Namespace:** `nukex`. Module-layer code is `pcl`.
- **No new third-party dependencies.** The fit is closed-form; do not reach for Eigen's solvers or add a new one.
- **Mono is a strict no-op.** A one-channel image must produce byte-identical output to today. `lrgb_mono_ngc7635` carries `"golden_frozen": true` and must not move.
- **`double` for the fit, `float` for pixels** — the existing rule across this codebase.
- **Naming departure from the spec, deliberate:** the spec calls the entry point `measure(...)`. A free function named `measure` in namespace `nukex` is too generic to live in a header everything includes. It is `measure_channel_transforms(...)` here. Nothing else in the spec's interface changes.
- **Console strings are read by users in the PixInsight Process Console.** ASCII only — PCL reads `const char*` as ISO-8859-1, so a UTF-8 character in a string literal becomes mojibake. No `±`, no `μ`, no en-dashes.
- **Build:** `cmake --build build -j$(nproc)` from the repo root (`build/` is already configured).
- **Test:** `ctest --test-dir build --output-on-failure`.

---

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

### Task 2: Measure the per-channel transform

The core of the feature. Given a frame and the star catalog the aligner already found on green, centroid every star in every channel and fit uniform scale plus translation per channel.

Coordinates are taken relative to the image centre, so `s` means radial magnification about the field centre and is nearly uncorrelated with the translations. That matters: the two effects being separated are one that scales with field radius (lateral colour) and one that does not (dispersion).

This task builds the happy path only. Degradation is Task 3.

**Files:**
- Create: `src/lib/alignment/include/nukex/alignment/channel_registration.hpp`
- Create: `src/lib/alignment/src/channel_registration.cpp`
- Modify: `src/lib/alignment/CMakeLists.txt`
- Create: `test/unit/alignment/test_channel_registration.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `nukex::default_reference_channel(int)` and `StarDetector::Config::channel` from Task 1; `nukex::Image`, `nukex::StarCatalog`, `nukex::Star`.
- Produces:
  - `struct nukex::ChannelTransform` with `double s, tx, ty`, `int n_stars`, `double residual`, `enum class Fit { Identity, TranslationOnly, Affine } fit`, and `bool is_identity(double tol = 1e-12) const`.
  - `struct nukex::ChannelTransforms` with `std::vector<ChannelTransform> per_channel`, `double cx, cy`, `int reference_channel`, `bool empty() const`, `bool negligible(double radius, double tol) const`.
  - `struct nukex::ChannelRegistrationConfig`.
  - `ChannelTransforms nukex::measure_channel_transforms(const Image&, const StarCatalog&, int reference_channel, const ChannelRegistrationConfig&)` plus a three-argument overload defaulting the config.

- [ ] **Step 1: Write the failing test**

Create `test/unit/alignment/test_channel_registration.cpp`:

```cpp
#include "catch_amalgamated.hpp"
#include "nukex/alignment/channel_registration.hpp"

#include <cmath>
#include <vector>

using namespace nukex;

namespace {

// A Gaussian star drawn at a sub-pixel position. Amplitude and sigma are
// fixed; what the tests vary is where it lands.
void draw_star(Image& img, int ch, double x, double y,
               double amplitude = 0.5, double sigma = 1.6) {
    const int r = 6;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int px = ix + dx, py = iy + dy;
            if (px < 0 || px >= img.width() || py < 0 || py >= img.height())
                continue;
            double ex = px - x, ey = py - y;
            img.at(px, py, ch) += static_cast<float>(
                amplitude * std::exp(-(ex * ex + ey * ey) / (2.0 * sigma * sigma)));
        }
    }
}

// A grid of star positions well inside the frame, avoiding the border so the
// centroid box always fits.
std::vector<std::pair<double, double>> star_grid(int w, int h, int n_side) {
    std::vector<std::pair<double, double>> out;
    for (int j = 0; j < n_side; j++)
        for (int i = 0; i < n_side; i++) {
            // The offsets keep centroids off exact integers, which is where a
            // broken centroid estimator would accidentally look correct.
            out.emplace_back(20.0 + 0.37 + i * (w - 40.0) / (n_side - 1),
                             20.0 + 0.61 + j * (h - 40.0) / (n_side - 1));
        }
    return out;
}

struct Synth {
    Image       image;
    StarCatalog catalog;
};

// Builds a 3-channel frame. Green carries stars at `pos`; red carries the same
// stars displaced by (s, tx, ty) about the image centre; blue matches green.
Synth make_frame(int w, int h, double s, double tx, double ty) {
    Synth out;
    out.image = Image(w, h, 3);
    out.image.fill(0.002f);

    const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;

    for (auto [x, y] : star_grid(w, h, 5)) {
        draw_star(out.image, 1, x, y);                 // green: reference
        draw_star(out.image, 2, x, y);                 // blue: agrees with green
        draw_star(out.image, 0,                        // red: displaced
                  s * (x - cx) + tx + cx,
                  s * (y - cy) + ty + cy);

        Star st;
        st.x = static_cast<float>(x);
        st.y = static_cast<float>(y);
        st.flux = 1.0f;
        out.catalog.stars.push_back(st);
    }
    return out;
}

} // namespace

TEST_CASE("measure_channel_transforms recovers a known scale and shift",
          "[channel_registration]") {
    // +500 ppm and a third of a pixel. At the corner of a 800x800 frame the
    // scale term alone is 500e-6 * 565 = 0.28 px, so both terms matter.
    const double s = 1.0005, tx = 0.33, ty = -0.21;
    Synth f = make_frame(800, 800, s, tx, ty);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    REQUIRE(ct.per_channel.size() == 3);
    REQUIRE(ct.reference_channel == 1);

    const ChannelTransform& red = ct.per_channel[0];
    REQUIRE(red.fit == ChannelTransform::Fit::Affine);
    REQUIRE(red.n_stars >= 20);

    // The acceptance the spec asks for is positional, so assert positionally:
    // the modelled position must land within 0.02 px of the truth at the
    // most demanding place, which is the field corner.
    const double R = std::hypot(399.5, 399.5);
    CHECK(std::abs(red.s - s) * R < 0.02);
    CHECK(std::abs(red.tx - tx) < 0.02);
    CHECK(std::abs(red.ty - ty) < 0.02);
    CHECK(red.residual < 0.02);
}

TEST_CASE("the reference channel is exactly identity",
          "[channel_registration]") {
    Synth f = make_frame(800, 800, 1.0005, 0.33, -0.21);
    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    const ChannelTransform& green = ct.per_channel[1];
    REQUIRE(green.fit == ChannelTransform::Fit::Identity);
    REQUIRE(green.is_identity());
    REQUIRE(green.s == 1.0);
    REQUIRE(green.tx == 0.0);
    REQUIRE(green.ty == 0.0);
}

TEST_CASE("a channel that already agrees measures as near-identity",
          "[channel_registration]") {
    Synth f = make_frame(800, 800, 1.0005, 0.33, -0.21);
    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    // Blue was drawn at the same positions as green. Its fit is not forced to
    // identity -- it is measured -- so it must come out small, not zero.
    const ChannelTransform& blue = ct.per_channel[2];
    const double R = std::hypot(399.5, 399.5);
    CHECK(std::abs(blue.s - 1.0) * R < 0.02);
    CHECK(std::abs(blue.tx) < 0.02);
    CHECK(std::abs(blue.ty) < 0.02);
}

TEST_CASE("a single-channel image registers nothing",
          "[channel_registration]") {
    Image mono(200, 200, 1);
    mono.fill(0.002f);
    StarCatalog cat;
    for (auto [x, y] : star_grid(200, 200, 4)) {
        draw_star(mono, 0, x, y);
        Star st; st.x = float(x); st.y = float(y); cat.stars.push_back(st);
    }

    ChannelTransforms ct = measure_channel_transforms(mono, cat, 0);
    REQUIRE(ct.empty());
}

TEST_CASE("negligible() is true only when every channel is near identity "
          "at the given radius", "[channel_registration]") {
    ChannelTransforms ct;
    ct.cx = 400; ct.cy = 400; ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[1].fit = ChannelTransform::Fit::Identity;

    // 1e-6 of scale over 565 px is 0.00056 px -- below any threshold worth
    // resampling for.
    ct.per_channel[0] = ChannelTransform{1.000001, 0.0, 0.0, 50, 0.01,
                                         ChannelTransform::Fit::Affine};
    CHECK(ct.negligible(565.0, 0.01));

    // 0.05 px of pure translation is not negligible at any radius.
    ct.per_channel[0].s  = 1.0;
    ct.per_channel[0].tx = 0.05;
    CHECK_FALSE(ct.negligible(565.0, 0.01));

    // 100 ppm is 0.056 px at the corner: negligible near the centre, not at
    // the edge. The radius argument is what makes that distinction.
    ct.per_channel[0].tx = 0.0;
    ct.per_channel[0].s  = 1.0001;
    CHECK_FALSE(ct.negligible(565.0, 0.01));
    CHECK(ct.negligible(50.0, 0.01));
}
```

- [ ] **Step 2: Register the test so it compiles**

In `test/CMakeLists.txt`, immediately after the `test_reference_selector` line:

```cmake
nukex_add_test(test_channel_registration unit/alignment/test_channel_registration.cpp nukex4_alignment)
```

- [ ] **Step 3: Run it and watch it fail**

```bash
cmake -S . -B build && cmake --build build -j$(nproc) --target test_channel_registration 2>&1 | tail -20
```

Expected: compile failure — `nukex/alignment/channel_registration.hpp` does not exist.

- [ ] **Step 4: Write the header**

Create `src/lib/alignment/include/nukex/alignment/channel_registration.hpp`:

```cpp
#pragma once

#include "nukex/alignment/types.hpp"
#include "nukex/alignment/star_detector.hpp"
#include "nukex/io/image.hpp"

#include <cmath>
#include <vector>

namespace nukex {

/// How one channel is displaced relative to the reference channel, within a
/// single frame.
///
/// This is NOT frame-to-frame alignment. It is the disagreement between the
/// colour planes of one exposure, which has two physical causes and no
/// correction anywhere else in the pipeline:
///
///   Lateral chromatic aberration -- the optics focus different wavelengths
///   at slightly different image scales. Fixed for a given rig, radial, zero
///   at the field centre and largest at the corners. Absorbed by `s`.
///
///   Atmospheric dispersion -- the atmosphere refracts blue more than red, by
///   an amount that depends on the target's altitude. It therefore DRIFTS
///   across a session, which is why this is measured per frame rather than
///   once on the stack. Absorbed by `tx`, `ty`.
///
/// Rotation is deliberately absent from the model. Neither effect rotates, so
/// a rotation term could only fit noise.
struct ChannelTransform {
    /// Which rung of the fallback ladder produced this transform. Reported to
    /// the user, so a frame that quietly degraded is visible rather than
    /// indistinguishable from one that fitted cleanly.
    enum class Fit { Identity, TranslationOnly, Affine };

    double s  = 1.0;   ///< uniform scale about the frame centre
    double tx = 0.0;   ///< translation in x, pixels
    double ty = 0.0;   ///< translation in y, pixels

    int    n_stars  = 0;     ///< stars that survived to the fit
    double residual = 0.0;   ///< median |measured - modelled|, pixels
    Fit    fit      = Fit::Identity;

    /// Exactly identity, in the sense of "there is nothing to apply".
    bool is_identity(double tol = 1e-12) const {
        return std::abs(s - 1.0) <= tol
            && std::abs(tx)      <= tol
            && std::abs(ty)      <= tol;
    }

    /// Largest displacement this transform produces within `radius` of the
    /// centre. The scale term grows with radius; the translation does not.
    double max_displacement(double radius) const {
        return std::abs(s - 1.0) * radius + std::hypot(tx, ty);
    }

    /// Map a point from reference-channel coordinates to where THIS channel
    /// images it, in the same frame. `cx`, `cy` must be the centre the fit was
    /// made about -- ChannelTransforms carries it for exactly this reason.
    void apply(double cx, double cy, float& x, float& y) const {
        const double nx = s * (static_cast<double>(x) - cx) + tx + cx;
        const double ny = s * (static_cast<double>(y) - cy) + ty + cy;
        x = static_cast<float>(nx);
        y = static_cast<float>(ny);
    }
};

/// One ChannelTransform per channel of a frame, plus the centre they share.
///
/// Empty means "nothing to do": a mono frame, or a frame with too few stars to
/// measure anything at all. Consumers must treat empty as the no-op path
/// rather than as a failure.
struct ChannelTransforms {
    std::vector<ChannelTransform> per_channel;
    double cx = 0.0;              ///< centre every fit is about, x
    double cy = 0.0;              ///< centre every fit is about, y
    int    reference_channel = 0;

    bool empty() const { return per_channel.empty(); }

    /// True when no channel moves any point within `radius` by more than
    /// `tol` pixels -- i.e. applying this would resample for nothing.
    bool negligible(double radius, double tol) const {
        for (const auto& t : per_channel)
            if (t.max_displacement(radius) > tol) return false;
        return true;
    }
};

struct ChannelRegistrationConfig {
    /// Half-width of the box each per-channel centroid is computed in.
    ///
    /// 6 gives a 13x13 box. This is NOT a comfort margin -- it was measured.
    /// Truncating a Gaussian star biases its centroid, and the bias does not
    /// cancel between channels because the same star lands on a different
    /// sub-pixel phase in each. Recovering a known +500 ppm and 0.33 px from
    /// a synthetic field, worst error across the three fitted parameters:
    ///
    ///     box radius:        4         5         6         7
    ///     FWHM 3.8 px:  0.0181    0.0035    0.0005    0.0000  px
    ///     FWHM 4.7 px:  0.0522    0.0192    0.0054    0.0012  px
    ///
    /// At radius 4 a perfectly ordinary 4.7 px star costs 0.05 px, which is
    /// half the entire acceptance budget spent on estimator bias before any
    /// real data is involved.
    int   centroid_radius = 6;

    /// Refine the box position and re-centroid this many times. The first
    /// pass centres the box on the reference channel's position, which for a
    /// displaced channel is off by the very thing being measured; each pass
    /// moves the box onto the measured centroid and shrinks the residual
    /// pull toward the box centre.
    ///
    /// Measured, with the faint case being the one that matters -- through a
    /// dual-narrowband filter a star can be five times brighter in Ha than in
    /// OIII, and that is where a single pass falls apart:
    ///
    ///     passes:              1         2
    ///     equal brightness: 0.0061    0.0118  px
    ///     red at 1/5 flux:  0.0569    0.0186  px
    ///
    /// Two costs a little on easy stars and saves a factor of three on hard
    /// ones. Do not raise it further; a third pass measurably regressed the
    /// clean case for no gain on the faint one.
    int   centroid_iterations = 2;

    /// A star with another catalog star closer than this is not used. The
    /// neighbour's wings intrude on the box and drag the centroid, and it
    /// does so by a different amount in each channel.
    ///
    /// Must exceed centroid_radius, or a neighbour sits inside the box by
    /// construction. Measured on a field where 30% of stars had a companion
    /// 7.6 px away: 0.029 px using every star, 0.006 px using only the
    /// isolated ones.
    ///
    /// Note this is a SEPARATE test from StarDetector::Config::exclusion_radius,
    /// which defaults to 5 and therefore permits exactly the neighbours that
    /// hurt here.
    int   min_neighbour_separation = 13;

    /// A star is used in a channel only when its peak in that channel stands
    /// this many sigma above the noise on the border of its own box. On a
    /// dual-narrowband frame the same star can be strong in red and invisible
    /// in blue, and centroiding noise would poison the fit.
    float min_star_snr = 3.0f;

    int   min_stars_affine      = 8;   ///< below this, drop to translation only
    int   min_stars_translation = 3;   ///< below this, give up and use identity

    /// A fitted scale further from 1 than this is not lateral colour. Real
    /// lateral colour runs a few hundred ppm; 1% is four orders of magnitude
    /// out and can only be a bad solve.
    double max_scale_deviation = 0.01;

    /// Likewise for translation. Atmospheric dispersion at these focal
    /// lengths is a fraction of a pixel.
    double max_translation_px = 5.0;

    /// Residuals beyond this many sigma are dropped and the fit repeated once.
    double clip_sigma = 3.0;
};

/// Fit one ChannelTransform per channel of `image`, against `reference_channel`.
///
/// `stars` are positions found on the reference channel -- normally the
/// catalog FrameAligner already computed, which is why this costs no extra
/// detection pass. Each star is re-centroided in each channel independently,
/// starting from its reference position.
///
/// Returns an empty ChannelTransforms for a single-channel image, or when the
/// catalog is empty. The reference channel's own entry is always exactly
/// identity, by construction rather than by fitting.
ChannelTransforms measure_channel_transforms(
    const Image& image, const StarCatalog& stars, int reference_channel,
    const ChannelRegistrationConfig& config);

inline ChannelTransforms measure_channel_transforms(
    const Image& image, const StarCatalog& stars, int reference_channel)
{
    return measure_channel_transforms(image, stars, reference_channel,
                                      ChannelRegistrationConfig{});
}

} // namespace nukex
```

- [ ] **Step 5: Write the implementation**

Create `src/lib/alignment/src/channel_registration.cpp`:

```cpp
#include "nukex/alignment/channel_registration.hpp"

#include <algorithm>
#include <cmath>

namespace nukex {
namespace {

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

struct Centroid {
    double x  = 0.0;
    double y  = 0.0;
    bool   ok = false;
};

/// Intensity-weighted centroid of `ch` in a box around (sx, sy).
///
/// The background is the MEDIAN of the box border, not its minimum. This is
/// the single most important choice in this file and it was arrived at by
/// measurement, not by taste.
///
/// The minimum of a noisy ring is an extreme order statistic: it is biased
/// low, and by an amount that varies box to box. Subtracting too little
/// leaves a pedestal under the star, and a pedestal pulls an
/// intensity-weighted centroid toward the geometric centre of the box --
/// which is the rounded integer position, so the pull depends on the star's
/// sub-pixel phase and does not cancel between channels. Measured on a
/// synthetic field with realistic noise, worst error over the fit:
///
///     background:        min      median
///     equal brightness: 0.034      0.006  px
///     red at 1/5 flux:  0.217      0.057  px
///
/// A factor of four, and on the faint channel a factor of four again.
///
/// The median is also what makes the neighbour case survivable: a companion
/// intruding on part of the ring moves fewer than half its pixels.
Centroid centroid_at(const Image& img, int ch, float sx, float sy,
                     int radius, int iterations, float min_snr) {
    const int w = img.width();
    const int h = img.height();

    double px_ = sx, py_ = sy;
    std::vector<double> ring;
    ring.reserve(8 * radius);

    for (int pass = 0; pass < std::max(1, iterations); pass++) {
        const int icx = static_cast<int>(std::lround(px_));
        const int icy = static_cast<int>(std::lround(py_));

        if (icx - radius < 0 || icx + radius >= w ||
            icy - radius < 0 || icy + radius >= h) {
            return {};   // box does not fit; unusable in every channel
        }

        ring.clear();
        for (int d = -radius; d <= radius; d++) {
            ring.push_back(img.at(icx + d, icy - radius, ch));
            ring.push_back(img.at(icx + d, icy + radius, ch));
        }
        for (int d = -radius + 1; d <= radius - 1; d++) {
            ring.push_back(img.at(icx - radius, icy + d, ch));
            ring.push_back(img.at(icx + radius, icy + d, ch));
        }

        const double bg = median_of(ring);

        // Noise from the ring's own robust spread, so the SNR gate below does
        // not need a global noise estimate the caller would have to supply.
        std::vector<double> dev;
        dev.reserve(ring.size());
        for (double v : ring) dev.push_back(std::abs(v - bg));
        const double sigma = 1.4826 * median_of(dev);

        double wsum = 0.0, wx = 0.0, wy = 0.0, peak = 0.0;
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                const int qx = icx + dx, qy = icy + dy;
                const double v = static_cast<double>(img.at(qx, qy, ch)) - bg;
                if (v <= 0.0) continue;
                wsum += v;
                wx   += v * qx;
                wy   += v * qy;
                peak  = std::max(peak, v);
            }
        }

        if (wsum <= 0.0) return {};

        // Reject a star this channel cannot see. A noiseless synthetic field
        // has sigma exactly zero, so guard that case rather than letting the
        // comparison pass by accident.
        if (sigma > 0.0 && peak < min_snr * sigma) return {};
        if (sigma <= 0.0 && peak <= 0.0)           return {};

        px_ = wx / wsum;
        py_ = wy / wsum;
    }

    return { px_, py_, true };
}

struct Pair {
    double u = 0.0, v = 0.0;      // reference-channel position, centre-relative
    double up = 0.0, vp = 0.0;    // this channel's position, centre-relative
};

/// Closed-form least squares for u' = s*u + tx, v' = s*v + ty with a single
/// shared s. Minimising the summed squared error in both axes gives
///
///     s  = [Suu' + Svv'] / [Suu + Svv]        (about the means)
///     tx = mean(u') - s * mean(u)
///
/// which needs no matrix solver, so this file has no Eigen dependency.
ChannelTransform fit_affine(const std::vector<Pair>& p) {
    ChannelTransform t;
    const double n = static_cast<double>(p.size());

    double su = 0, sv = 0, sup = 0, svp = 0;
    for (const auto& q : p) { su += q.u; sv += q.v; sup += q.up; svp += q.vp; }
    const double mu = su / n, mv = sv / n, mup = sup / n, mvp = svp / n;

    double num = 0, den = 0;
    for (const auto& q : p) {
        num += (q.u - mu) * (q.up - mup) + (q.v - mv) * (q.vp - mvp);
        den += (q.u - mu) * (q.u  - mu)  + (q.v - mv) * (q.v  - mv);
    }

    // den is the spread of the stars about their own centroid. It vanishes
    // only if every star sits at one point, which the caller's star count
    // makes impossible in practice, but a division by it must still be safe.
    t.s  = (den > 1e-9) ? (num / den) : 1.0;
    t.tx = mup - t.s * mu;
    t.ty = mvp - t.s * mv;
    t.fit = ChannelTransform::Fit::Affine;
    return t;
}

ChannelTransform fit_translation(const std::vector<Pair>& p) {
    ChannelTransform t;
    const double n = static_cast<double>(p.size());
    double dx = 0, dy = 0;
    for (const auto& q : p) { dx += q.up - q.u; dy += q.vp - q.v; }
    t.s  = 1.0;
    t.tx = dx / n;
    t.ty = dy / n;
    t.fit = ChannelTransform::Fit::TranslationOnly;
    return t;
}

std::vector<double> residuals_of(const ChannelTransform& t,
                                 const std::vector<Pair>& p) {
    std::vector<double> r;
    r.reserve(p.size());
    for (const auto& q : p) {
        const double mx = t.s * q.u + t.tx;
        const double my = t.s * q.v + t.ty;
        r.push_back(std::hypot(q.up - mx, q.vp - my));
    }
    return r;
}

} // namespace

ChannelTransforms measure_channel_transforms(
    const Image& image, const StarCatalog& stars, int reference_channel,
    const ChannelRegistrationConfig& config)
{
    ChannelTransforms out;

    const int nch = image.n_channels();
    if (nch < 2 || image.empty() || stars.empty()) return out;
    if (reference_channel < 0 || reference_channel >= nch) return out;

    out.cx = (image.width()  - 1) / 2.0;
    out.cy = (image.height() - 1) / 2.0;
    out.reference_channel = reference_channel;
    out.per_channel.assign(nch, ChannelTransform{});

    // Isolation. A neighbour inside the centroid box drags the centroid, and
    // by a different amount in each channel, so a crowded star is worse than
    // no star. StarDetector's exclusion_radius (5 by default) is smaller than
    // the centroid box, so it does NOT already guarantee this -- the filter
    // has to be here.
    //
    // O(n^2) over the catalog, which caps at max_stars (200 by default), so
    // 40000 comparisons per frame against tens of millions of pixels. Not
    // worth a spatial index.
    const double min_sep2 = static_cast<double>(config.min_neighbour_separation)
                          * config.min_neighbour_separation;
    std::vector<bool> isolated(stars.stars.size(), true);
    for (size_t i = 0; i < stars.stars.size(); i++) {
        for (size_t j = i + 1; j < stars.stars.size(); j++) {
            const double dx = stars.stars[i].x - stars.stars[j].x;
            const double dy = stars.stars[i].y - stars.stars[j].y;
            if (dx * dx + dy * dy < min_sep2) {
                isolated[i] = false;
                isolated[j] = false;
            }
        }
    }

    // Reference positions, centroided on the reference channel itself rather
    // than taken from the catalog. The catalog's centroids came from a
    // different estimator with a different aperture; using this one for both
    // sides means any bias it has cancels instead of leaking into the fit.
    struct RefPos { double x, y; bool ok; };
    std::vector<RefPos> ref(stars.stars.size());
    for (size_t i = 0; i < stars.stars.size(); i++) {
        if (!isolated[i]) { ref[i] = {0.0, 0.0, false}; continue; }
        const Star& s = stars.stars[i];
        const Centroid c = centroid_at(image, reference_channel, s.x, s.y,
                                       config.centroid_radius,
                                       config.centroid_iterations,
                                       config.min_star_snr);
        ref[i] = { c.x, c.y, c.ok };
    }

    for (int ch = 0; ch < nch; ch++) {
        if (ch == reference_channel) {
            out.per_channel[ch] = ChannelTransform{};   // identity by construction
            out.per_channel[ch].n_stars =
                static_cast<int>(std::count_if(ref.begin(), ref.end(),
                                               [](const RefPos& r) { return r.ok; }));
            continue;
        }

        std::vector<Pair> pairs;
        pairs.reserve(stars.stars.size());
        for (size_t i = 0; i < stars.stars.size(); i++) {
            if (!ref[i].ok) continue;
            const Star& s = stars.stars[i];
            const Centroid c = centroid_at(image, ch, s.x, s.y,
                                           config.centroid_radius,
                                           config.centroid_iterations,
                                           config.min_star_snr);
            if (!c.ok) continue;
            pairs.push_back({ ref[i].x - out.cx, ref[i].y - out.cy,
                              c.x      - out.cx, c.y      - out.cy });
        }

        out.per_channel[ch] = fit_channel(pairs, config);
    }

    return out;
}

} // namespace nukex
```

Note the call to `fit_channel(pairs, config)` at the end — that is the ladder, and it is written in Task 3. For **this** task, to get the test green with the minimum code, define it in the anonymous namespace above `measure_channel_transforms` as the affine path only:

```cpp
/// Task 2: happy path only. Task 3 replaces this with the full ladder.
ChannelTransform fit_channel(const std::vector<Pair>& pairs,
                             const ChannelRegistrationConfig& config) {
    ChannelTransform t;
    if (static_cast<int>(pairs.size()) < config.min_stars_affine) return t;
    t = fit_affine(pairs);
    t.n_stars  = static_cast<int>(pairs.size());
    t.residual = median_of(residuals_of(t, pairs));
    return t;
}
```

- [ ] **Step 6: Add the source to the library**

In `src/lib/alignment/CMakeLists.txt`, add to `add_library(nukex4_alignment STATIC ...)`, after `reference_selector.cpp`:

```cmake
    src/channel_registration.cpp
```

- [ ] **Step 7: Run the test and watch it pass**

```bash
cmake --build build -j$(nproc) --target test_channel_registration && ./build/test/test_channel_registration
```

Expected: all five cases PASS.

If the recovery assertions fail by a small margin, the cause is almost always the centroid box: check `centroid_radius` against the drawn `sigma = 1.6`, which puts real signal out to about 4 px. Do not loosen the tolerance to make it pass — 0.02 px is the number the acceptance criterion depends on.

- [ ] **Step 8: Commit**

```bash
git add src/lib/alignment/include/nukex/alignment/channel_registration.hpp \
        src/lib/alignment/src/channel_registration.cpp \
        src/lib/alignment/CMakeLists.txt \
        test/unit/alignment/test_channel_registration.cpp \
        test/CMakeLists.txt
git commit -m "feat(alignment): measure per-channel scale and shift

Centroids every star in every channel and fits uniform scale plus
translation against the reference channel, about the frame centre so
scale means radial magnification. Closed-form least squares, no solver.
Recovers a known +500 ppm and 0.33 px to within 0.02 px at the corner.

Happy path only; the fallback ladder is next."
```

---

### Task 3: The fallback ladder

Every way this can degrade, named and tested. A frame that cannot be measured must be stacked as it is, never guessed at.

**Files:**
- Modify: `src/lib/alignment/src/channel_registration.cpp` (replace `fit_channel`)
- Test: `test/unit/alignment/test_channel_registration.cpp` (append)

**Interfaces:**
- Consumes: everything from Task 2.
- Produces: no new symbols. `ChannelTransform::fit` now takes all three enumerator values, and out-of-range fits return `Fit::Identity`.

- [ ] **Step 1: Write the failing tests**

Append to `test/unit/alignment/test_channel_registration.cpp`:

```cpp
// --- the fallback ladder -------------------------------------------------

namespace {

// A frame with exactly `n` usable stars in every channel, red displaced.
Synth make_sparse_frame(int w, int h, int n, double s, double tx, double ty) {
    Synth out;
    out.image = Image(w, h, 3);
    out.image.fill(0.002f);
    const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;

    for (int i = 0; i < n; i++) {
        // Spread along a diagonal so the positions are never degenerate.
        const double x = 30.0 + 0.37 + i * (w - 60.0) / std::max(1, n - 1);
        const double y = 30.0 + 0.61 + i * (h - 60.0) / std::max(1, n - 1);
        draw_star(out.image, 1, x, y);
        draw_star(out.image, 2, x, y);
        draw_star(out.image, 0, s * (x - cx) + tx + cx, s * (y - cy) + ty + cy);
        Star st; st.x = float(x); st.y = float(y); out.catalog.stars.push_back(st);
    }
    return out;
}

} // namespace

TEST_CASE("too few stars for four parameters falls back to translation only",
          "[channel_registration]") {
    // 5 stars: below min_stars_affine (8), above min_stars_translation (3).
    Synth f = make_sparse_frame(600, 600, 5, 1.0, 0.4, -0.25);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    REQUIRE(red.fit == ChannelTransform::Fit::TranslationOnly);
    CHECK(red.s == 1.0);
    CHECK(std::abs(red.tx - 0.4)  < 0.05);
    CHECK(std::abs(red.ty + 0.25) < 0.05);
}

TEST_CASE("too few stars for translation falls back to identity",
          "[channel_registration]") {
    Synth f = make_sparse_frame(600, 600, 2, 1.0, 0.4, -0.25);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    REQUIRE(red.fit == ChannelTransform::Fit::Identity);
    REQUIRE(red.is_identity());
}

TEST_CASE("no stars at all yields empty transforms",
          "[channel_registration]") {
    Image img(400, 400, 3);
    img.fill(0.002f);
    ChannelTransforms ct = measure_channel_transforms(img, StarCatalog{}, 1);
    REQUIRE(ct.empty());
}

TEST_CASE("an implausible scale is rejected rather than applied",
          "[channel_registration]") {
    // 5% is five hundred times any real lateral colour. A fit this far out is
    // a bad solve, and applying it would wreck the frame.
    Synth f = make_frame(800, 800, 1.05, 0.0, 0.0);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    REQUIRE(red.fit == ChannelTransform::Fit::Identity);
    REQUIRE(red.is_identity());
}

TEST_CASE("an implausible translation is rejected rather than applied",
          "[channel_registration]") {
    Synth f = make_frame(800, 800, 1.0, 9.0, 0.0);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    REQUIRE(red.fit == ChannelTransform::Fit::Identity);
    REQUIRE(red.is_identity());
}

TEST_CASE("a star invisible in one channel is dropped from that channel's fit "
          "and kept in the others", "[channel_registration]") {
    // The dual-narrowband case: red is Ha, blue is OIII, and plenty of stars
    // are strong in one and absent from the other.
    const double s = 1.0005, tx = 0.30, ty = -0.20;
    Synth f = make_frame(800, 800, s, tx, ty);

    // Erase half the red stars. Red must still fit -- from the survivors.
    const double cx = 399.5, cy = 399.5;
    int erased = 0;
    for (size_t i = 0; i < f.catalog.stars.size(); i += 2) {
        const Star& st = f.catalog.stars[i];
        const int rx = int(std::lround(s * (st.x - cx) + tx + cx));
        const int ry = int(std::lround(s * (st.y - cy) + ty + cy));
        for (int dy = -7; dy <= 7; dy++)
            for (int dx = -7; dx <= 7; dx++) {
                int px = rx + dx, py = ry + dy;
                if (px < 0 || px >= 800 || py < 0 || py >= 800) continue;
                f.image.at(px, py, 0) = 0.002f;
            }
        erased++;
    }
    REQUIRE(erased > 0);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);

    const ChannelTransform& red = ct.per_channel[0];
    REQUIRE(red.fit == ChannelTransform::Fit::Affine);
    CHECK(red.n_stars < int(f.catalog.stars.size()));
    CHECK(red.n_stars > 0);
    const double R = std::hypot(cx, cy);
    CHECK(std::abs(red.s - s) * R < 0.03);

    // Blue was untouched and must have used every star.
    CHECK(ct.per_channel[2].n_stars == int(f.catalog.stars.size()));
}

TEST_CASE("a star with a close neighbour is excluded from the fit",
          "[channel_registration]") {
    // StarDetector's exclusion_radius is 5 by default and the centroid box is
    // 13 wide, so the detector hands over stars whose boxes overlap. Their
    // centroids get dragged by the neighbour, and by a different amount in
    // each channel, so they have to be dropped here.
    const double s = 1.0005, tx = 0.30, ty = -0.20;
    Synth f = make_frame(800, 800, s, tx, ty);
    const double cx = 399.5, cy = 399.5;

    // Give the first three stars a companion 8 px away, in every channel, and
    // put the companions in the catalog -- that is what the detector would do.
    const size_t n_before = f.catalog.stars.size();
    std::vector<std::pair<double, double>> companions;
    for (size_t i = 0; i < 3; i++)
        companions.emplace_back(f.catalog.stars[i].x + 8.0,
                                f.catalog.stars[i].y + 0.0);
    for (auto [ox, oy] : companions) {
        draw_star(f.image, 1, ox, oy, 0.3);
        draw_star(f.image, 2, ox, oy, 0.3);
        draw_star(f.image, 0, s * (ox - cx) + tx + cx, s * (oy - cy) + ty + cy, 0.3);
        Star st; st.x = float(ox); st.y = float(oy); f.catalog.stars.push_back(st);
    }

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    // Both members of each crowded pair go: 3 originals + 3 companions.
    REQUIRE(red.n_stars == int(n_before) - 3);

    // And the fit is still good, because the isolated stars carry it.
    const double R = std::hypot(cx, cy);
    CHECK(std::abs(red.s - s) * R < 0.02);
    CHECK(std::abs(red.tx - tx) < 0.02);
}

TEST_CASE("an outlier centroid is clipped out of the fit",
          "[channel_registration]") {
    const double s = 1.0005, tx = 0.30, ty = -0.20;
    Synth f = make_frame(800, 800, s, tx, ty);

    // Move one red star far from where the model says it should be, the way a
    // cosmic ray hit or a blended neighbour would.
    const double cx = 399.5, cy = 399.5;
    const Star& st = f.catalog.stars[3];
    const int rx = int(std::lround(s * (st.x - cx) + tx + cx));
    const int ry = int(std::lround(s * (st.y - cy) + ty + cy));
    draw_star(f.image, 0, rx + 3.0, ry + 3.0, 2.0);

    ChannelTransforms ct = measure_channel_transforms(f.image, f.catalog, 1);
    const ChannelTransform& red = ct.per_channel[0];

    // Without clipping, one 4 px outlier among 25 stars drags the fit well
    // past 0.02 px. With clipping it barely registers.
    const double R = std::hypot(cx, cy);
    CHECK(std::abs(red.s - s) * R < 0.02);
    CHECK(std::abs(red.tx - tx) < 0.02);
    CHECK(red.n_stars < int(f.catalog.stars.size()));
}
```

- [ ] **Step 2: Run and watch them fail**

```bash
cmake --build build -j$(nproc) --target test_channel_registration && ./build/test/test_channel_registration
```

Expected: the five new cases about the ladder FAIL. Specifically the translation-only case reports `Fit::Identity` (Task 2's `fit_channel` returns identity below 8 stars), and both implausible-fit cases report `Fit::Affine` with the absurd value applied. Confirm you see those exact failures before writing code — they are what proves the tests bite.

- [ ] **Step 3: Replace `fit_channel` with the full ladder**

In `src/lib/alignment/src/channel_registration.cpp`, replace the Task 2 `fit_channel` with:

```cpp
/// The fallback ladder. Every rung is named in the returned Fit so the console
/// can report which one a frame landed on.
///
///   enough stars       -> affine, sigma-clipped once
///   fewer than that    -> translation only
///   fewer still        -> identity
///   implausible result -> identity
///
/// Rejection is always to identity, never to a partial correction. A wrong
/// transform is worse than none: it moves every pixel of a channel.
ChannelTransform fit_channel(const std::vector<Pair>& pairs,
                             const ChannelRegistrationConfig& config) {
    ChannelTransform t;   // identity
    const int n = static_cast<int>(pairs.size());

    if (n < config.min_stars_translation) return t;

    auto plausible = [&](const ChannelTransform& c) {
        return std::abs(c.s - 1.0)  <= config.max_scale_deviation
            && std::abs(c.tx)       <= config.max_translation_px
            && std::abs(c.ty)       <= config.max_translation_px
            && std::isfinite(c.s) && std::isfinite(c.tx) && std::isfinite(c.ty);
    };

    if (n >= config.min_stars_affine) {
        ChannelTransform a = fit_affine(pairs);

        // One sigma-clip pass. A second buys nothing measurable and risks
        // eating real signal at the field edges, which is precisely where the
        // correction is largest and least redundant.
        std::vector<double> r = residuals_of(a, pairs);
        const double med = median_of(r);
        std::vector<double> dev;
        dev.reserve(r.size());
        for (double v : r) dev.push_back(std::abs(v - med));
        // 1.4826 * MAD estimates sigma for a normal distribution.
        const double sigma = 1.4826 * median_of(dev);

        if (sigma > 0.0) {
            const double cut = med + config.clip_sigma * sigma;
            std::vector<Pair> kept;
            kept.reserve(pairs.size());
            for (size_t i = 0; i < pairs.size(); i++)
                if (r[i] <= cut) kept.push_back(pairs[i]);

            if (static_cast<int>(kept.size()) >= config.min_stars_affine
                && kept.size() < pairs.size()) {
                a = fit_affine(kept);
                a.n_stars  = static_cast<int>(kept.size());
                a.residual = median_of(residuals_of(a, kept));
                return plausible(a) ? a : ChannelTransform{};
            }
        }

        a.n_stars  = n;
        a.residual = med;
        if (plausible(a)) return a;
        // An implausible affine fit does not earn a translation-only retry:
        // the same bad centroids feed it. Fall through to identity.
        return ChannelTransform{};
    }

    ChannelTransform tr = fit_translation(pairs);
    tr.n_stars  = n;
    tr.residual = median_of(residuals_of(tr, pairs));
    return plausible(tr) ? tr : ChannelTransform{};
}
```

- [ ] **Step 4: Run and watch them pass**

```bash
cmake --build build -j$(nproc) --target test_channel_registration && ./build/test/test_channel_registration
```

Expected: all thirteen cases PASS, including the five from Task 2.

- [ ] **Step 5: Run the whole suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: all green.

- [ ] **Step 6: Commit**

```bash
git add src/lib/alignment/src/channel_registration.cpp \
        test/unit/alignment/test_channel_registration.cpp
git commit -m "feat(alignment): fallback ladder for channel registration

Affine when there are stars for it, translation only when there are not,
identity when there are barely any, and identity again when the fit comes
back implausible. One sigma-clip pass on the residuals. Rejection is
always to identity: a wrong transform moves every pixel of a channel, so
it is worse than no transform at all.

Covers the dual-narrowband case where a star is bright in Ha and absent
in OIII -- it drops out of that channel's fit and stays in the other's."
```

---

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

### Task 6: Report it in the Process Console

A user reading the console must be able to tell that channel registration happened, which rung of the ladder each channel landed on, and how big the correction was. A silent correction is indistinguishable from a bug.

**Files:**
- Modify: `src/lib/stacker/src/stacking_engine.cpp` (the Phase A alignment log, around line 652)
- Test: `test/unit/alignment/test_channel_registration.cpp` (append — the formatter is pure and testable)

**Interfaces:**
- Consumes: `AlignedFrame::channels`, `ChannelTransform::fit`, `ChannelTransform::max_displacement`.
- Produces: `std::string nukex::describe_channel_transforms(const ChannelTransforms&, double radius)` declared in `channel_registration.hpp`.

- [ ] **Step 1: Write the failing test**

Append to `test/unit/alignment/test_channel_registration.cpp`:

```cpp
// --- console reporting ---------------------------------------------------

TEST_CASE("describe_channel_transforms names the rung and the size",
          "[channel_registration]") {
    ChannelTransforms ct;
    ct.cx = 399.5; ct.cy = 399.5; ct.reference_channel = 1;
    ct.per_channel.resize(3);
    ct.per_channel[0] = ChannelTransform{1.000238, 0.31, -0.12, 187, 0.043,
                                         ChannelTransform::Fit::Affine};
    ct.per_channel[1].fit = ChannelTransform::Fit::Identity;
    ct.per_channel[2] = ChannelTransform{1.0, 0.02, -0.01, 190, 0.031,
                                         ChannelTransform::Fit::TranslationOnly};

    const std::string s = describe_channel_transforms(ct, 565.0);

    // The rung, the size of the correction, and the star count all have to be
    // there: a user seeing a moved golden needs to know why from the log.
    CHECK(s.find("ch0") != std::string::npos);
    CHECK(s.find("affine") != std::string::npos);
    CHECK(s.find("238 ppm") != std::string::npos);
    CHECK(s.find("n=187") != std::string::npos);
    CHECK(s.find("ch2") != std::string::npos);
    CHECK(s.find("translation") != std::string::npos);
    CHECK(s.find("ch1") == std::string::npos);   // the reference is not reported

    // ASCII only. PCL reads const char* as ISO-8859-1, so a stray UTF-8 byte
    // reaches the Process Console as mojibake.
    for (unsigned char c : s) CHECK(c < 0x80);
}

TEST_CASE("describe_channel_transforms says so when there is nothing to say",
          "[channel_registration]") {
    CHECK(describe_channel_transforms(ChannelTransforms{}, 565.0).empty());
}
```

- [ ] **Step 2: Run and watch it fail**

```bash
cmake --build build -j$(nproc) --target test_channel_registration 2>&1 | tail -20
```

Expected: compile failure — `describe_channel_transforms` is not declared.

- [ ] **Step 3: Declare and implement the formatter**

In `channel_registration.hpp`, after `measure_channel_transforms`:

```cpp
/// One line describing what channel registration did, for the Process
/// Console. Empty when there was nothing to report.
///
/// `radius` is where the displacement is quoted -- normally the frame corner,
/// which is where a scale term is largest and where a user looking at their
/// stars will notice.
///
/// ASCII only: PCL reads const char* as ISO-8859-1, so a UTF-8 character here
/// reaches the console as mojibake.
std::string describe_channel_transforms(const ChannelTransforms& ct,
                                        double radius);
```

Add `#include <string>` to the header.

In `channel_registration.cpp`:

```cpp
std::string describe_channel_transforms(const ChannelTransforms& ct,
                                        double radius) {
    if (ct.empty()) return {};

    std::string out;
    for (size_t ch = 0; ch < ct.per_channel.size(); ch++) {
        if (static_cast<int>(ch) == ct.reference_channel) continue;
        const ChannelTransform& t = ct.per_channel[ch];

        char buf[192];
        switch (t.fit) {
        case ChannelTransform::Fit::Affine:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu affine %+.0f ppm t=(%+.2f,%+.2f) "
                          "n=%d res=%.3fpx max=%.3fpx",
                          ch, (t.s - 1.0) * 1e6, t.tx, t.ty,
                          t.n_stars, t.residual, t.max_displacement(radius));
            break;
        case ChannelTransform::Fit::TranslationOnly:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu translation t=(%+.2f,%+.2f) n=%d res=%.3fpx",
                          ch, t.tx, t.ty, t.n_stars, t.residual);
            break;
        case ChannelTransform::Fit::Identity:
            std::snprintf(buf, sizeof(buf),
                          "ch%zu identity (nothing measurable)", ch);
            break;
        }
        if (!out.empty()) out += "; ";
        out += buf;
    }
    return out;
}
```

Add `#include <cstdio>` and `#include <string>` to the .cpp.

- [ ] **Step 4: Run and watch it pass**

```bash
cmake --build build -j$(nproc) --target test_channel_registration && ./build/test/test_channel_registration
```

Expected: PASS.

- [ ] **Step 5: Emit it from the engine**

In `src/lib/stacker/src/stacking_engine.cpp`, immediately after the `obs.advance(0, "  aligned: " + status + ...)` block closes (the `else` branch ending around line 680), add:

```cpp
        // Channel registration, when there was any. Silence here means the
        // frame's channels already agreed to better than 0.01 px at the
        // corner, which is the near-identity skip doing its job.
        if (!aligned.channels.empty()) {
            const double corner = std::hypot(aligned.image.width()  / 2.0,
                                             aligned.image.height() / 2.0);
            const std::string desc =
                describe_channel_transforms(aligned.channels, corner);
            if (!desc.empty()) obs.advance(0, "  channel reg: " + desc);
        }
```

Add `#include "nukex/alignment/channel_registration.hpp"` to the engine's includes if it is not already reachable through `frame_aligner.hpp` (it is, but name it explicitly — this file uses the symbol directly).

- [ ] **Step 6: Build and run the suite**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build --output-on-failure
```

Expected: all green.

- [ ] **Step 7: Commit**

```bash
git add src/lib/alignment/include/nukex/alignment/channel_registration.hpp \
        src/lib/alignment/src/channel_registration.cpp \
        src/lib/stacker/src/stacking_engine.cpp \
        test/unit/alignment/test_channel_registration.cpp
git commit -m "feat(stacker): report channel registration in the console

Names the rung, the size in ppm and pixels, the star count and the
residual, per channel. Silence means the near-identity skip took the
plain path. ASCII only -- PCL reads const char* as ISO-8859-1."
```

---

### Task 7: Re-baseline the goldens and measure the acceptance criterion

Two changes in this plan move Bayer output: star detection now runs on green (Task 1), and the channels are now registered (Tasks 2-5). Both were expected. This task proves the frozen mono golden did **not** move, re-baselines the two that did, and measures the number the spec set as the bar.

**Files:**
- Modify: `test/fixtures/golden/bayer_rgb_m27_2023.json`
- Modify: `test/fixtures/golden/bayer_nb_hao3_m16.json`
- Verify unchanged: `test/fixtures/golden/lrgb_mono_ngc7635.json`
- Create: `tools/measure_channel_registration.py`
- Modify: `src/module/NukeXVersion.h` (version bump)

**Interfaces:**
- Consumes: the whole feature.
- Produces: nothing other code depends on.

- [ ] **Step 1: Run the frozen mono case first, before anything else**

`lrgb_mono_ngc7635` carries `"golden_frozen": true`. A one-channel stack must take the no-op path at every step of this feature, so its golden must be bit-identical. Prove that before touching any other golden — if it moved, the mono no-op is broken and nothing else in this task is meaningful.

```bash
NUKEX_E2E_ONLY=lrgb_mono_ngc7635 cmake --build build --target e2e 2>&1 | tail -40
node tools/validate_e2e.js
```

`run_e2e.sh` takes only `regen` as a positional argument; a single case is
selected with the `NUKEX_E2E_ONLY` environment variable. The full corpus is
four stacks of real data and takes hours, so run one case at a time.

Expected: PASS with no golden drift. If it drifted, stop and find out why. The likely cause is `default_reference_channel` being consulted somewhere with the wrong channel count, or `saturation_fraction` having been changed in Task 1 despite the instruction not to.

- [ ] **Step 2: Run the two Bayer cases and capture the new baselines**

```bash
NUKEX_E2E_ONLY=bayer_rgb_m27_2023 cmake --build build --target e2e 2>&1 | tail -60
NUKEX_E2E_ONLY=bayer_nb_hao3_m16  cmake --build build --target e2e 2>&1 | tail -60
```

These are long runs (budgets are 10800 s and 7200 s in the manifest). Read the console output for the new `channel reg:` lines — they are the first real-data evidence the feature is working, and they must show a plausible correction rather than a rejected one. A field of `identity (nothing measurable)` across every frame means the star SNR gate is too tight for real data; investigate before accepting the baselines.

Also confirm `min_frames_ok_alignment` still holds for both cases. Detecting on green rather than red should if anything **improve** alignment on OSC data, since green has twice the photosites. A drop is a regression, not an expected re-baseline.

- [ ] **Step 3: Update the two golden files**

Regenerate rather than hand-editing hashes:

```bash
NUKEX_E2E_ONLY=bayer_rgb_m27_2023 cmake --build build --target e2e-regen
NUKEX_E2E_ONLY=bayer_nb_hao3_m16  cmake --build build --target e2e-regen
```

Then `git diff test/fixtures/golden/` and read what moved before staging it. A
re-baseline you have not looked at is indistinguishable from a regression you
have accepted. Record in the commit message that this is deliberate and why.

- [ ] **Step 4: Write the acceptance measurement**

Create `tools/measure_channel_registration.py`:

```python
#!/usr/bin/env python3
"""Median red-to-green and blue-to-green star separation in a stacked XISF.

The acceptance criterion for per-channel registration. On the M3 set before
the feature: R-G 0.435 px, B-G 0.058 px. B-G is the floor -- it is centroid
noise, not colour error, because through an L-Quad Enhance green and blue are
both imaging near 500 nm. The bar is R-G below 0.10 px.

Usage: measure_channel_registration.py <stacked.xisf>
"""
import sys
import numpy as np


def read_planes(path):
    """Return (R, G, B) float32 planes from an XISF file."""
    # The project already reads XISF elsewhere; reuse that path rather than
    # writing a third parser. If nothing is importable, convert once with
    # PixInsight and read a FITS instead -- the measurement is what matters,
    # not the container.
    raise NotImplementedError(
        "wire this to the project's existing XISF reader before running")


def centroid(plane, x, y, r=4):
    y0, y1, x0, x1 = y - r, y + r + 1, x - r, x + r + 1
    if y0 < 0 or x0 < 0 or y1 > plane.shape[0] or x1 > plane.shape[1]:
        return None
    box = plane[y0:y1, x0:x1].astype(np.float64)
    bg = min(box[0].min(), box[-1].min(), box[:, 0].min(), box[:, -1].min())
    w = np.clip(box - bg, 0, None)
    if w.sum() <= 0:
        return None
    gy, gx = np.mgrid[y0:y1, x0:x1]
    return (w * gx).sum() / w.sum(), (w * gy).sum() / w.sum()


def main(path):
    R, G, B = read_planes(path)

    # Peaks in green, at the 99.9th percentile -- the same 245-star sample the
    # spec's numbers come from, so the before and after are comparable.
    thresh = np.percentile(G, 99.9)
    ys, xs = np.where(G >= thresh)

    seen, stars = set(), []
    for y, x in zip(ys, xs):
        key = (y // 16, x // 16)      # one star per 16x16 cell, brightest wins
        if key in seen:
            continue
        seen.add(key)
        stars.append((int(x), int(y)))

    for name, plane in (("R-G", R), ("B-G", B)):
        d = []
        for x, y in stars:
            a, b = centroid(G, x, y), centroid(plane, x, y)
            if a and b:
                d.append(np.hypot(b[0] - a[0], b[1] - a[1]))
        d = np.array(d)
        print(f"{name}: n={len(d)}  median={np.median(d):.3f} px  "
              f"mean={d.mean():.3f} px")


if __name__ == "__main__":
    main(sys.argv[1])
```

The `read_planes` stub is deliberate and must be filled in by whoever runs this — wiring it to the project's existing XISF path is a two-line change, and guessing at that path here would be worse than saying so.

- [ ] **Step 5: Measure the M3 set**

Re-stack `/home/scarter4work/projects/processing/M3` with the new build and run the script on `NukeX_stacked`.

| measured on the shipped stack, 1047 stars | |
|---|---|
| red vs green | 0.354 px |
| blue vs green, the floor | 0.081 px |
| ratio today | **4.4x** |
| **bar** | **ratio at or below 1.5x** |

The bar is a ratio rather than an absolute pixel figure, deliberately. An
absolute threshold encodes whichever centroid estimator measured it, and this
document's first draft proved the point: it quoted 0.435 and 0.058 from an
earlier estimator and a smaller sample. Measuring blue on the same image in
the same run gives a floor that moves with the estimator, so the ratio stays
meaningful when the absolute numbers do not.

Also check green's median star FWHM against the previous stack. Green is never resampled, so it must be unchanged. A change there means the reference channel is being warped, which would be a bug in Task 5's near-identity path.

- [ ] **Step 6: Version bump, sign, package**

Per the release workflow in CLAUDE.md, in this order, none skipped:

In `src/module/NukeXVersion.h`, currently 5.0.1.0, bump
`NUKEX_MODULE_VERSION_BUILD` to 1 and set `NUKEX_MODULE_RELEASE_YEAR`,
`_MONTH` and `_DAY` to today. Then:

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
cmake --build build --target package     # signs the module and the XRI
```

`package` signs the module, builds the tarball, updates the SHA1 in
`updates.xri` and signs that too. The version bump and the package files are
committed together, in one commit, and only then pushed.

Do **not** `sudo` anything into `/opt/PixInsight`. That directory is owned by `scarter4work` and group-writable; a root-owned file there makes PixInsight's own updater fail with `Permission denied` and leaves a half-applied update. If the E2E harness needs a module in `<PI>/bin`, install it without elevation.

- [ ] **Step 7: Commit**

```bash
git add test/fixtures/golden/bayer_rgb_m27_2023.json \
        test/fixtures/golden/bayer_nb_hao3_m16.json \
        tools/measure_channel_registration.py \
        src/module/
git commit -m "test(e2e): re-baseline the Bayer goldens for channel registration

Both Bayer cases move for two reasons, both intended: star detection now
runs on green rather than channel 0, and the colour channels are now
registered to each other within each frame.

lrgb_mono_ngc7635 does NOT move and was verified first. It is frozen, a
one-channel stack takes the no-op path at every step of this feature, and
it is the anchor proving nothing else drifted.

Acceptance measured on the M3 set: median red-to-green separation, against
0.435 px before and a 0.058 px floor."
```

---

## Self-Review

**Spec coverage.** Every section of the design document maps to a task:

| spec section | task |
|---|---|
| Where it sits (measure between homography and warp) | 5 |
| Green is the reference (detector + registration) | 1, 2 |
| The model (uniform scale + translation, no rotation) | 2 |
| Interface (`ChannelTransform`, `ChannelTransforms`, measure) | 2 |
| `warp` overload applying `H · A_c` | 4 |
| Failure ladder, all six rows | 3 (rows 1-4), 2 (isolation), 5 (near-identity skip) |
| Testing: synthetic and exact | 2 |
| Testing: the ladder | 3 |
| Testing: integration through the engine | 5 |
| Testing: acceptance on real data | 7 |
| Goldens move / frozen mono does not | 7 |
| Decided: independent per-channel fits | 2 (each channel fitted in its own loop iteration) |

**The near-identity skip lands in Task 5 rather than 3**, deliberately: it needs the frame's dimensions to know the corner radius, and `measure_channel_transforms` does not see a decision about resampling.

**The spec's isolation row is implemented as its own filter, and an earlier draft of this plan was wrong about that.** That draft argued `StarDetector::Config::exclusion_radius` already keeps detections apart, so the catalog arrives isolated by construction and no separate pass was needed. It does not: `exclusion_radius` defaults to 5 and the centroid box is 13 wide, so the detector permits exactly the neighbours that hurt. Simulating a field where 30% of stars had a companion 7.6 px away put the fit error at 0.029 px using every star and 0.006 px using only the isolated ones. `min_neighbour_separation` exists because that claim was tested and failed.

**Type consistency.** `ChannelTransform`, `ChannelTransforms`, `ChannelRegistrationConfig`, `measure_channel_transforms`, `describe_channel_transforms`, `default_reference_channel`, `ChannelTransform::Fit`, `ChannelTransform::apply`, `ChannelTransform::max_displacement`, `ChannelTransforms::negligible`, `FrameAligner::Config::register_channels`, `FrameAligner::Config::channel_config`, `AlignedFrame::channels` — each is defined in exactly one task and spelled identically at every later use. The aggregate initialisation `ChannelTransform{1.000238, 0.31, -0.12, 187, 0.043, Fit::Affine}` used in the Task 3 and Task 6 tests matches the member order declared in Task 2: `s, tx, ty, n_stars, residual, fit`.

**One ordering hazard, called out here because it is easy to trip on.** Task 2's implementation calls `fit_channel`, which Task 3 replaces. Task 2 must define the simple version in the anonymous namespace *above* `measure_channel_transforms`, or it will not compile. Task 3's step 3 replaces that definition in place rather than adding a second one.
