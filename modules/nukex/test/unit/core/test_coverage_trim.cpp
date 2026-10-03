#include "catch_amalgamated.hpp"
#include "nukex/core/coverage_trim.hpp"
#include "nukex/core/cube.hpp"
#include <string>

using namespace nukex;

namespace {

ChannelConfig config(std::initializer_list<const char*> names) {
    ChannelConfig c;
    c.n_channels = static_cast<uint8_t>(names.size());
    int i = 0;
    for (const char* n : names) c.channel_names[i++] = n;
    return c;
}

/// A cube where every slot of every pixel saw `frames` frames.
Cube full_cube(int w, int h, const ChannelConfig& cfg, int frames) {
    Cube cube(w, h, cfg);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < cfg.n_channels; ++c)
                for (int f = 0; f < frames; ++f)
                    cube.at(x, y).channel(c).welford.update(1.0f);
    return cube;
}

void drop(Cube& cube, int c, int x, int y, int n) {
    auto& w = cube.at(x, y).channel(c).welford;
    w.reset();
    for (int f = 0; f < n; ++f) w.update(1.0f);
}

} // namespace

TEST_CASE("Nothing is trimmed when every frame covered every pixel",
          "[core][trim]") {
    const ChannelConfig cfg = config({"R", "G", "B"});
    Cube cube = full_cube(16, 12, cfg, 5);

    const TrimBounds t = full_coverage_rect(cube);
    REQUIRE_FALSE(t.applied);
    REQUIRE(t.x0 == 0);
    REQUIRE(t.y0 == 0);
    REQUIRE(t.width()  == 16);
    REQUIRE(t.height() == 12);
}

TEST_CASE("A short border is trimmed off exactly", "[core][trim]") {
    // The dither-and-drift rim: the outer ring saw fewer frames than the
    // interior, so intersection mode cuts it away.
    const ChannelConfig cfg = config({"R", "G", "B"});
    Cube cube = full_cube(16, 12, cfg, 5);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x)
            if (x < 2 || x > 13 || y < 2 || y > 9)
                for (int c = 0; c < 3; ++c) drop(cube, c, x, y, 4);

    const TrimBounds t = full_coverage_rect(cube);
    REQUIRE(t.applied);
    REQUIRE(t.x0 == 2);
    REQUIRE(t.y0 == 2);
    REQUIRE(t.x1 == 13);
    REQUIRE(t.y1 == 9);
}

TEST_CASE("A pixel short in ONE channel is not fully covered", "[core][trim]") {
    // Per-channel registration can push one colour plane off the source while
    // the others stay on it -- that is exactly how the user's stack ended up
    // with a dead red column and complete green and blue ones. The output
    // pixel is broken either way, so any channel falling short disqualifies it.
    const ChannelConfig cfg = config({"R", "G", "B"});
    Cube cube = full_cube(16, 12, cfg, 5);
    for (int y = 0; y < 12; ++y) drop(cube, 0, 15, y, 0);   // red only, last column

    const TrimBounds t = full_coverage_rect(cube);
    REQUIRE(t.applied);
    REQUIRE(t.x1 == 14);
    REQUIRE(t.width() == 15);
    REQUIRE(t.height() == 12);
}

TEST_CASE("A slot no frame ever filled does not trim the whole frame",
          "[core][trim]") {
    // An L-only stack still allocates the colour slots its channel config
    // names. Treating "zero frames anywhere" as "zero coverage everywhere"
    // would reduce the output to nothing.
    const ChannelConfig cfg = config({"L", "R", "G", "B"});
    Cube cube(16, 12, cfg);
    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 16; ++x)
            for (int f = 0; f < 5; ++f)
                cube.at(x, y).channel(0).welford.update(1.0f);   // L only

    const TrimBounds t = full_coverage_rect(cube);
    REQUIRE_FALSE(t.applied);
    REQUIRE(t.width()  == 16);
    REQUIRE(t.height() == 12);
}

TEST_CASE("The largest complete rectangle wins, not the first one found",
          "[core][trim]") {
    // Real coverage is not a rectangle: rotation and meridian flips leave a
    // ragged edge. Walking inward from each side until the edge line happens
    // to be clean can stop early and keep an incomplete pixel; the answer has
    // to be the largest rectangle that is entirely covered.
    const ChannelConfig cfg = config({"R"});
    Cube cube = full_cube(20, 10, cfg, 5);
    // A notch biting into the left half only.
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x) drop(cube, 0, x, y, 3);

    const TrimBounds t = full_coverage_rect(cube);
    REQUIRE(t.applied);
    // Two candidates: the full-width strip below the notch (20 x 4 = 80) or
    // the full-height block to its right (12 x 10 = 120). The block wins.
    REQUIRE(t.width()  == 12);
    REQUIRE(t.height() == 10);
    REQUIRE(t.x0 == 8);
    REQUIRE(t.y0 == 0);
}
