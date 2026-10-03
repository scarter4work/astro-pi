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

