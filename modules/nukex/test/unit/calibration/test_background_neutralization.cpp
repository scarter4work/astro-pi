#include "catch_amalgamated.hpp"
#include "nukex/calibration/background_neutralization.hpp"
#include "nukex/core/channel_config.hpp"
#include <initializer_list>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace nukex;

namespace {

// Deterministic, well-spread noise (see test_tier1.cpp for why a plain
// coordinate hash is not good enough for a robust-statistics test).
float gauss(std::uint64_t i) {
    auto mix = [](std::uint64_t z) {
        z += 0x9E3779B97F4A7C15ull;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    const double u1 = (mix(2 * i) >> 11) * 0x1.0p-53 + 1e-12;
    const double u2 = (mix(2 * i + 1) >> 11) * 0x1.0p-53;
    return static_cast<float>(std::sqrt(-2.0 * std::log(u1)) *
                              std::cos(6.283185307179586 * u2));
}

/// An OSC broadband frame carrying the pedestals measured on the user's own
/// 74-frame stack -- R 0.0270, G 0.0418, B 0.0388, a 1.54:1 green cast -- with
/// a deliberately NEUTRAL signal on top. Any colour in the output is the
/// pedestal, not the target.
Image cast_frame() {
    const float ped[3] = {0.0270f, 0.0418f, 0.0388f};
    const float sigma = 0.0002f;
    Image img(128, 128, 3);
    for (int ch = 0; ch < 3; ++ch) {
        float* d = img.channel_data(ch);
        for (int i = 0; i < 128 * 128; ++i) {
            d[i] = ped[ch] + sigma * gauss(static_cast<std::uint64_t>(ch * 100003 + i));
            if (i % 1000 == 0) d[i] += 0.02f;   // neutral "stars"
        }
    }
    return img;
}

float median_of(const Image& img, int ch) {
    std::vector<float> v(img.channel_data(ch),
                         img.channel_data(ch) + img.width() * img.height());
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    return v[v.size() / 2];
}

} // namespace

TEST_CASE("Background matching removes an additive channel cast",
          "[calibration][background]") {
    // The green cast in a broadband stack is an additive sky pedestal, not a
    // scaling error: measured on the user's stack the background sits at
    // G/R = 1.54 while the signal, once each channel's own background is
    // removed, sits at G/R = 1.07. A stretch preserves colour ratios, so the
    // pedestal survives to the final image at full strength -- it has to come
    // off in linear space.
    Image img = cast_frame();
    const BackgroundOffsets off = neutralize_channel_backgrounds(img);

    REQUIRE(off.applied);
    const float r = median_of(img, 0), g = median_of(img, 1), b = median_of(img, 2);
    INFO("R=" << r << " G=" << g << " B=" << b);
    REQUIRE(g / r == Catch::Approx(1.0f).margin(0.01f));
    REQUIRE(b / r == Catch::Approx(1.0f).margin(0.01f));
}

TEST_CASE("Background matching leaves the dimmest channel alone",
          "[calibration][background]") {
    // Matching UP would invent light; matching down to the dimmest channel is
    // the physically honest statement -- the extra green IS extra sky. It also
    // guarantees nothing is driven negative.
    Image img = cast_frame();
    const Image before = img.clone();
    const BackgroundOffsets off = neutralize_channel_backgrounds(img);

    REQUIRE(off.subtracted.size() == 3);
    REQUIRE(off.subtracted[0] == Catch::Approx(0.0f).margin(1e-6f));
    REQUIRE(off.subtracted[1] > 0.014f);
    for (int i = 0; i < img.width() * img.height(); ++i)
        REQUIRE(img.channel_data(0)[i] == before.channel_data(0)[i]);
}

TEST_CASE("Background matching preserves the signal's own colour",
          "[calibration][background]") {
    // The point is to remove the pedestal WITHOUT touching the target. A
    // constant offset per channel cannot change a difference, and this is the
    // test that says so out loud.
    Image img = cast_frame();
    const Image before = img.clone();
    neutralize_channel_backgrounds(img);

    for (int ch = 0; ch < 3; ++ch) {
        const float m_before = median_of(before, ch);
        const float m_after  = median_of(img, ch);
        // Star pixel 1000 minus its own channel background: unchanged.
        const float sig_before = before.channel_data(ch)[1000] - m_before;
        const float sig_after  = img.channel_data(ch)[1000] - m_after;
        INFO("channel " << ch);
        REQUIRE(sig_after == Catch::Approx(sig_before).margin(1e-6f));
    }
}

TEST_CASE("Background matching never drives a pixel negative",
          "[calibration][background]") {
    // The dimmest channel is the reference, so the only pixels that could go
    // negative are ones already below their own channel's background by more
    // than the offset -- the deepest of the read noise.
    Image img = cast_frame();
    neutralize_channel_backgrounds(img);
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < img.width() * img.height(); ++i)
            REQUIRE(img.channel_data(ch)[i] >= 0.0f);
}

TEST_CASE("Background matching is a no-op on a mono frame",
          "[calibration][background]") {
    Image img(64, 64, 1);
    for (int i = 0; i < 64 * 64; ++i)
        img.channel_data(0)[i] = 0.03f + 0.0002f * gauss(static_cast<std::uint64_t>(i));
    const Image before = img.clone();

    const BackgroundOffsets off = neutralize_channel_backgrounds(img);
    REQUIRE_FALSE(off.applied);
    for (int i = 0; i < 64 * 64; ++i)
        REQUIRE(img.channel_data(0)[i] == before.channel_data(0)[i]);
}

TEST_CASE("Background matching is a no-op on an already-neutral frame",
          "[calibration][background]") {
    // The narrowband path composes a background that last release measured
    // neutral to five decimals. Running this over it must not disturb it.
    Image img(64, 64, 3);
    for (int ch = 0; ch < 3; ++ch)
        for (int i = 0; i < 64 * 64; ++i)
            img.channel_data(ch)[i] =
                0.03f + 0.0002f * gauss(static_cast<std::uint64_t>(ch * 7919 + i));

    const BackgroundOffsets off = neutralize_channel_backgrounds(img);
    for (float s : off.subtracted) REQUIRE(s < 0.0005f);
}

TEST_CASE("Background matching works on bare planes, for the composer's slots",
          "[calibration][background]") {
    // The derived-slot path holds its planes as loose vectors rather than an
    // Image, and the composed window has to agree with the other two.
    std::vector<float> r(1024, 0.0270f), g(1024, 0.0418f), b(1024, 0.0388f);
    const std::vector<float*> planes = {r.data(), g.data(), b.data()};
    const BackgroundOffsets off = match_plane_backgrounds(planes, r.size());

    REQUIRE(off.applied);
    REQUIRE(g[0] == Catch::Approx(0.0270f).margin(1e-6f));
    REQUIRE(b[0] == Catch::Approx(0.0270f).margin(1e-6f));
    REQUIRE(r[0] == Catch::Approx(0.0270f).margin(1e-6f));
}

TEST_CASE("Only the chroma slots are matched", "[calibration][background]") {
    // The cast is a CHROMA problem: R, G and B have to agree with each other.
    // Luminance does not belong in that set -- an L filter is broad, its sky
    // level is not comparable to a single colour channel's, and pulling it
    // down to match one would darken the image for no colour gain. Emission
    // slots are excluded outright: Ha and OIII are different physical lines,
    // not a colour balance, and last release measured that path's background
    // neutral to five decimals already.
    auto names = [](std::initializer_list<const char*> ns) {
        ChannelConfig c;
        c.n_channels = static_cast<uint8_t>(ns.size());
        int i = 0;
        for (const char* n : ns) c.channel_names[i++] = n;
        return c;
    };

    REQUIRE(chroma_slot_indices(names({"R", "G", "B", "L"})) ==
            std::vector<int>{0, 1, 2});
    REQUIRE(chroma_slot_indices(names({"L", "R", "G", "B"})) ==
            std::vector<int>{1, 2, 3});
    REQUIRE(chroma_slot_indices(names({"Ha", "OIII"})).empty());
    REQUIRE(chroma_slot_indices(names({"L"})).empty());
    // Dual-narrowband slots are decomposed into emission lines downstream;
    // they are not a colour balance either.
    REQUIRE(chroma_slot_indices(names({"R_HaO3", "G_HaO3", "B_HaO3"})).empty());
}
