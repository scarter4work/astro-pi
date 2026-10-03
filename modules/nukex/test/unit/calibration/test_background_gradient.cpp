#include "catch_amalgamated.hpp"
#include "nukex/calibration/background_gradient.hpp"
#include "nukex/io/image.hpp"

#include <cmath>
#include <random>

using namespace nukex;

// A stacked frame carries a sky gradient that no per-frame normalisation can
// remove. Measured on the user's 65-frame NGC7635 set: the per-frame TILT is
// the same in every frame to within 0.05-0.10 of a frame's own noise, while
// the per-frame LEVEL varies by 2.3x that. So the tilt is a fixed pattern --
// optics, flats, or a static light-pollution direction -- and survives
// stacking intact. In the stack it spans about 6 sigma corner to corner.
//
// The danger in removing it is eating real signal, so these tests pin both
// halves: it must find a real gradient, and it must NOT invent one from an
// object sitting on flat sky.

namespace {

Image ramp_image(int w, int h, float level, float dx, float dy, float sigma,
                 unsigned seed = 7) {
    Image img(w, h, 1);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.0f, sigma);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float fx = static_cast<float>(x) / w - 0.5f;
            const float fy = static_cast<float>(y) / h - 0.5f;
            img.at(x, y, 0) = level + dx * fx + dy * fy + (sigma > 0 ? g(rng) : 0.0f);
        }
    return img;
}

float median_of(const Image& img, int ch) {
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(img.width()) * img.height());
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++) v.push_back(img.at(x, y, ch));
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

} // namespace

TEST_CASE("BackgroundGradient: recovers a known planar tilt", "[gradient]") {
    Image img = ramp_image(128, 96, 0.20f, 0.006f, 0.015f, 0.0005f);
    auto m = BackgroundGradient::fit_planar(img, 0);
    REQUIRE(m.valid);
    REQUIRE(m.dx == Catch::Approx(0.006f).epsilon(0.10));
    REQUIRE(m.dy == Catch::Approx(0.015f).epsilon(0.10));
}

TEST_CASE("BackgroundGradient: flat sky yields no tilt", "[gradient]") {
    Image img = ramp_image(128, 96, 0.20f, 0.0f, 0.0f, 0.0005f);
    auto m = BackgroundGradient::fit_planar(img, 0);
    REQUIRE(m.valid);
    REQUIRE(m.level == Catch::Approx(0.20f).margin(0.002f));
    REQUIRE(std::fabs(m.dx) < 0.0005f);
    REQUIRE(std::fabs(m.dy) < 0.0005f);
}

TEST_CASE("BackgroundGradient: a bright object does not drag the fit", "[gradient]") {
    // The failure mode that matters. A nebula filling a third of the frame
    // sits ENTIRELY on one side, so a symmetric fit tilts toward it and the
    // subtraction then carves a wedge out of real signal.
    Image img = ramp_image(128, 96, 0.20f, 0.0f, 0.0f, 0.0005f);
    for (int y = 20; y < 76; y++)
        for (int x = 80; x < 124; x++)
            img.at(x, y, 0) += 0.15f;

    auto m = BackgroundGradient::fit_planar(img, 0);
    REQUIRE(m.valid);
    REQUIRE(m.level == Catch::Approx(0.20f).margin(0.002f));
    REQUIRE(std::fabs(m.dx) < 0.002f);
    REQUIRE(std::fabs(m.dy) < 0.002f);
}

TEST_CASE("BackgroundGradient: subtraction flattens the sky", "[gradient]") {
    Image img = ramp_image(128, 96, 0.20f, 0.006f, 0.015f, 0.0005f);
    auto m = BackgroundGradient::fit_planar(img, 0);
    REQUIRE(m.valid);
    REQUIRE(m.dy == Catch::Approx(0.015f).epsilon(0.10));   // the tilt was there
    BackgroundGradient::subtract(img, 0, m);

    auto after = BackgroundGradient::fit_planar(img, 0);
    REQUIRE(std::fabs(after.dx) < 0.0005f);
    REQUIRE(std::fabs(after.dy) < 0.0005f);
}

TEST_CASE("BackgroundGradient: subtraction preserves the sky level", "[gradient]") {
    // Only the TILT comes off. Moving the overall level would shift the
    // stretch's sky target and change how bright the image looks, which is a
    // different decision and not this one.
    Image img = ramp_image(128, 96, 0.20f, 0.006f, 0.015f, 0.0005f);
    const float before = median_of(img, 0);
    const float corner_before = img.at(4, 4, 0);
    auto m = BackgroundGradient::fit_planar(img, 0);
    REQUIRE(m.valid);
    BackgroundGradient::subtract(img, 0, m);
    // the level holds, but the corner must actually have moved
    REQUIRE(median_of(img, 0) == Catch::Approx(before).margin(0.001f));
    REQUIRE(std::fabs(img.at(4, 4, 0) - corner_before) > 0.002f);
}

TEST_CASE("BackgroundGradient: object flux survives subtraction", "[gradient]") {
    // Same object, with a real gradient underneath. After flattening, the
    // object's contrast against its IMMEDIATE surroundings must be intact.
    //
    // The comparison pixel has to be close. Two points far apart legitimately
    // move relative to each other -- the gradient between them is exactly what
    // is being removed -- so a distant reference would assert that flattening
    // does not flatten.
    Image img = ramp_image(128, 96, 0.20f, 0.006f, 0.015f, 0.0005f);
    for (int y = 40; y < 56; y++)
        for (int x = 56; x < 72; x++) img.at(x, y, 0) += 0.10f;

    const float before = img.at(64, 54, 0) - img.at(64, 58, 0);  // 4 px apart
    auto m = BackgroundGradient::fit_planar(img, 0);
    REQUIRE(m.valid);
    REQUIRE(BackgroundGradient::amplitude(m) > 0.005);
    BackgroundGradient::subtract(img, 0, m);
    const float after = img.at(64, 54, 0) - img.at(64, 58, 0);
    REQUIRE(after == Catch::Approx(before).margin(0.002f));
}

// ── The failure that was measured, not imagined ──────────────────────────
//
// A bright object with its own ramp, on one side of the frame, dragged the
// previous fit at 30% coverage: fitted dx +0.26 where the truth was 0. The
// residual scatter was measured on ALL samples, so the object inflated it
// until the clip could no longer exclude the object. The fit now seeds from
// the darkest samples and measures scatter on what it keeps.

namespace {
Image sky_with_object(int w, int h, float dx, float dy, double cover, unsigned seed = 11) {
    Image img = ramp_image(w, h, 0.20f, dx, dy, 0.0005f, seed);
    const int x_obj = static_cast<int>(w * (1.0 - cover));
    for (int y = 0; y < h; y++)
        for (int x = x_obj; x < w; x++)
            img.at(x, y, 0) += 0.15f + 0.10f * static_cast<float>(x - x_obj) / (w - x_obj);
    return img;
}
} // namespace

TEST_CASE("BackgroundGradient: a bright ramped object covering up to 80% of the "
          "frame does not drag the fit", "[gradient]") {
    for (double cover : {0.30, 0.55, 0.80}) {
        for (bool tilted : {false, true}) {
            const float dx = tilted ? 0.006f : 0.0f, dy = tilted ? 0.015f : 0.0f;
            Image img = sky_with_object(256, 192, dx, dy, cover);
            auto m = BackgroundGradient::fit_planar(img, 0);
            INFO("cover " << cover << " tilted " << tilted);
            REQUIRE(m.valid);
            REQUIRE(m.dx == Catch::Approx(dx).margin(0.002f));
            REQUIRE(m.dy == Catch::Approx(dy).margin(0.002f));
            REQUIRE(m.level == Catch::Approx(0.20f).margin(0.003f));
        }
    }
}

TEST_CASE("BackgroundGradient: sampling is restricted to the region, so a dark "
          "noisy rim outside it cannot tilt the fit", "[gradient]") {
    // The stack's thin-coverage rim: one-sided here (drift toward one edge),
    // darker and ten times noisier than the interior, at maximum leverage.
    const int W = 256, H = 192, RIM = 24;
    Image img = ramp_image(W, H, 0.20f, 0.0f, 0.0f, 0.0005f);
    std::mt19937 rng(5);
    std::normal_distribution<float> loud(0.0f, 0.005f);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < RIM; x++)
            img.at(x, y, 0) = 0.197f + loud(rng);

    const FitRegion interior{RIM, 0, W - 1, H - 1};
    auto inside = BackgroundGradient::fit_planar(img, 0, &interior);
    REQUIRE(inside.valid);
    REQUIRE(std::fabs(inside.dx) < 0.0005f);
    REQUIRE(std::fabs(inside.dy) < 0.0005f);
    // The model is still expressed over the FULL frame, so the level is the
    // frame-centre sky, not the region's.
    REQUIRE(inside.level == Catch::Approx(0.20f).margin(0.001f));

    // Without the region the rim is sampled; whatever it does, it must not be
    // what the interior-only fit does, or the region would be pointless.
    auto whole = BackgroundGradient::fit_planar(img, 0);
    REQUIRE(std::fabs(whole.dx) > std::fabs(inside.dx));
}

TEST_CASE("BackgroundGradient: collinear samples cannot define a plane and are "
          "reported invalid", "[gradient]") {
    // Every non-zero sample on one row: the normal matrix is singular.
    Image img(128, 96, 1);
    for (int y = 0; y < 96; y++)
        for (int x = 0; x < 128; x++) img.at(x, y, 0) = 0.0f;
    for (int x = 0; x < 128; x++) img.at(x, 40, 0) = 0.2f;
    auto m = BackgroundGradient::fit_planar(img, 0);
    REQUIRE_FALSE(m.valid);
}

TEST_CASE("BackgroundGradient: amplitude is the corner-to-corner range", "[gradient]") {
    GradientModel m; m.valid = true; m.dx = 1.0e-3; m.dy = -1.0e-3;
    // The brightest and darkest corners differ by |dx| + |dy|, not hypot.
    REQUIRE(BackgroundGradient::amplitude(m) == Catch::Approx(2.0e-3));
    GradientModel none;
    REQUIRE(BackgroundGradient::amplitude(none) == 0.0);
}
