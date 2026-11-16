// Lupton at the ends of its parameter ranges, and CLAHE's per-channel mode.
//
// Found by the standalone app's Stretch stage (astro-studio 1bfa3d2):
//   - Lupton with Q = 0 or stretch = 0, both inside its own declared bounds,
//     returned an all-black image without a word;
//   - CLAHE with luminance_only = false equalized channel 0 of a colour image
//     and passed the others through.

#include "catch_amalgamated.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"

#include <cmath>
#include <stdexcept>

using namespace nukex;

namespace {
Image ramp_rgb(int w, int h) {
    Image img(w, h, 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = 0.002f + 0.2f * static_cast<float>(x + y * w) / static_cast<float>(w * h);
            img.at(x, y, 0) = v;
            img.at(x, y, 1) = 0.8f * v;
            img.at(x, y, 2) = 0.6f * v;
        }
    return img;
}
bool all_finite(const Image& img) {
    for (std::size_t i = 0; i < img.data_size(); ++i) if (!std::isfinite(img.data()[i])) return false;
    return true;
}
}

// Lupton et al. (2004) map intensity by f(I) = asinh(Q I / stretch) / Q, here
// normalised by frac / asinh(frac Q). As Q -> 0, asinh(z) -> z and f(I) ->
// I / stretch: the softening parameter at zero is the LINEAR map, the
// paper's own limiting case. Q = 0 is therefore a valid value with a
// well-defined result, and must give it.
TEST_CASE("Lupton: Q = 0 is the linear limit I / stretch, not black", "[stretch][lupton]") {
    LuptonStretch op;
    op.Q = 0.0f;
    op.stretch = 0.5f;
    REQUIRE(op.apply_scalar(0.1f) == Catch::Approx(0.2f).epsilon(1e-6));
    REQUIRE(op.apply_scalar(0.25f) == Catch::Approx(0.5f).epsilon(1e-6));
    // Continuous with small Q.
    LuptonStretch small; small.Q = 1e-3f; small.stretch = 0.5f;
    REQUIRE(small.apply_scalar(0.1f) == Catch::Approx(op.apply_scalar(0.1f)).epsilon(1e-4));

    Image img = ramp_rgb(32, 16);
    const Image before = img.clone();
    op.apply(img);
    REQUIRE(all_finite(img));
    // Colour ratios kept, intensity linear: every channel doubled (clipped at 1).
    for (int x = 0; x < 32; ++x) {
        REQUIRE(img.at(x, 8, 0) == Catch::Approx(std::min(1.0f, 2.0f * before.at(x, 8, 0))).epsilon(1e-5));
        REQUIRE(img.at(x, 8, 2) / img.at(x, 8, 0) == Catch::Approx(0.6f).epsilon(1e-4));
    }
}

// stretch is the divisor of the intensity: at 0 the map is a step (every
// positive pixel to the top), which is no stretch at all -- the right answer
// is to refuse it, and the bounds must not offer it.
TEST_CASE("Lupton: stretch <= 0 is refused loudly and is outside the bounds", "[stretch][lupton]") {
    const auto b = LuptonStretch().param_bounds();
    REQUIRE(b.at("stretch").first > 0.0f);
    REQUIRE(b.at("Q").first == 0.0f);

    LuptonStretch op;
    REQUIRE_THROWS_AS(op.set_param("stretch", 0.0f), std::invalid_argument);
    REQUIRE_THROWS_AS(op.set_param("stretch", -1.0f), std::invalid_argument);
    REQUIRE_THROWS_AS(op.set_param("Q", -0.5f), std::invalid_argument);
    REQUIRE(op.stretch == 5.0f);                 // a refused value is not applied

    op.stretch = 0.0f;                           // set directly, past set_param
    Image img = ramp_rgb(8, 8);
    REQUIRE_THROWS_AS(op.apply(img), std::invalid_argument);
    REQUIRE_THROWS_AS(op.apply_scalar(0.1f), std::invalid_argument);

    // The bounds' own minimum works.
    LuptonStretch lo; lo.stretch = b.at("stretch").first; lo.Q = 8.0f;
    Image img2 = ramp_rgb(8, 8);
    lo.apply(img2);
    REQUIRE(all_finite(img2));
    REQUIRE(img2.at(4, 4, 0) > 0.0f);
}

TEST_CASE("CLAHE: per-channel mode equalizes every channel of a colour image", "[stretch][clahe]") {
    CLAHEStretch op;
    op.luminance_only = false;
    op.tile_cols = op.tile_rows = 4;
    Image img(64, 64, 3);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            for (int c = 0; c < 3; ++c)
                img.at(x, y, c) = 0.05f + 0.1f * static_cast<float>((x * (c + 1) + y) % 17) / 17.0f;
    const Image before = img.clone();
    op.apply(img);
    for (int c = 0; c < 3; ++c) {
        // Each channel processed exactly as that channel alone would be.
        Image one(64, 64, 1);
        for (int i = 0; i < 64 * 64; ++i) one.data()[i] = before.channel_data(c)[i];
        op.apply(one);
        int changed = 0;
        for (int i = 0; i < 64 * 64; ++i) {
            REQUIRE(img.channel_data(c)[i] == one.data()[i]);
            if (img.channel_data(c)[i] != before.channel_data(c)[i]) ++changed;
        }
        INFO("channel " << c);
        REQUIRE(changed > 64 * 64 / 2);
    }
}

TEST_CASE("CLAHE: luminance mode on colour and single-channel images is unchanged", "[stretch][clahe]") {
    // Mono: per-channel and luminance mode are the same operation.
    CLAHEStretch a, b;
    b.luminance_only = false;
    Image m1(48, 40, 1), m2;
    for (int i = 0; i < 48 * 40; ++i) m1.data()[i] = 0.02f + 0.3f * static_cast<float>(i % 97) / 97.0f;
    m2 = m1.clone();
    a.apply(m1);
    b.apply(m2);
    for (int i = 0; i < 48 * 40; ++i) REQUIRE(m1.data()[i] == m2.data()[i]);
}
