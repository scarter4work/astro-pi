// Task 10: screen blend and STF preview pixels (display.py), end to end from the port's own
// float64 render and nebula grade (the goldens were made from Python's float64 results, so the
// 8-bit preview is compared exactly).
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/Display.h"
#include "core/HaloProfile.h"
#include "core/Nebula.h"
#include "core/RenderStars.h"
#include "core/Transform.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using gdg::Image;
using gdg::test::golden;
using gdg::test::golden_image;

namespace {

const gdg::Gains kDialog{0.65, 0.85, 0.12, 0.4, 1.0};

std::vector<gdg::StarRow> table(const std::string& prefix) {
    const auto x = golden(prefix + "_x").data, y = golden(prefix + "_y").data, f = golden(prefix + "_flux").data,
               r = golden(prefix + "_r_med_geo").data;
    std::vector<gdg::StarRow> t;
    for (size_t i = 0; i < x.size(); ++i) {
        gdg::StarRow row{x[i], y[i], f[i]};
        row.r_med = r[i];
        t.push_back(row);
    }
    return t;
}

struct Case {
    std::string stars, starless, table, effective, blend, u8;
    gdg::Gains gains;
    bool own_halo;
    double strength, atmos, structure;
    std::optional<double> budget;
};

// The generator's chain: render (gains, halo) -> nebula (first setting) -> blend -> autostretch.
Image blend_of(const Case& c) {
    const Image stars = golden_image(c.stars);
    const auto t = table(c.table);
    const gdg::Modulation m = gdg::compute_modulation(golden(c.effective).data, c.gains);
    gdg::HaloProfile halo;
    if (c.own_halo) halo = gdg::build_halo_profile(stars, t, 2.0, gdg::GradeConfig{}, gdg::Log{});
    const Image rendered = gdg::render_stars(stars, t, m, 2.0, c.own_halo ? &halo : nullptr, 1.0);
    const Image neb = gdg::render_nebula(golden_image(c.starless), t, c.strength, c.atmos, c.structure, c.budget);
    return gdg::screen_blend(rendered, neb);
}

const std::vector<Case>& cases() {
    static const std::vector<Case> cs = {
        {"stars_rgb", "starless_rgb", "match_table", "effective", "screen_blend", "autostretch_u8", kDialog, true, 1.0, 1.0,
         1.0, std::nullopt},
        {"mono_stars", "mono_starless", "mono_match_table", "mono_effective", "mono_screen_blend", "mono_autostretch_u8",
         gdg::Gains{}, true, 2.0, 0.5, 1.5, std::nullopt},
        {"odd_stars", "odd_starless", "odd_table", "odd_effective", "odd_screen_blend", "odd_autostretch_u8", kDialog, false,
         2.0, 0.5, 1.5, 0.8},
    };
    return cs;
}

}  // namespace

TEST_CASE("screen_blend_matches", "[display][golden]") {
    for (const Case& c : cases()) {
        const Image got = blend_of(c);
        const Image want = golden_image(c.blend);
        REQUIRE(got.same_shape(want));
        double d = 0.0;
        size_t unequal = 0;
        for (size_t i = 0; i < got.data.size(); ++i) {
            d = std::max(d, std::fabs(got.data[i] - want.data[i]));
            if (static_cast<double>(static_cast<float>(got.data[i])) != want.data[i]) ++unequal;
        }
        WARN(c.blend << ": max |delta| " << d << " vs the float32 golden, " << unequal << " of " << got.data.size()
                     << " not float32-identical");
        CHECK(d <= 6e-8);
        CHECK(unequal == 0);
    }
    Image a(2, 1, 1), b(2, 1, 1);
    a.data = {0.25, 1.0};
    b.data = {0.5, 0.3};
    CHECK(gdg::screen_blend(a, b).data == std::vector<double>{1.0 - 0.75 * 0.5, 1.0});
    a.data = {0.1, 0.14};
    b.data = {0.7, 0.95};
    // display.screen_blend literals: 1 - (1 - a)(1 - b) rounds differently from a + b - ab here.
    CHECK(gdg::screen_blend(a, b).data == std::vector<double>{0x1.75c28f5c28f5cp-1, 0x1.e9fbe76c8b439p-1});
    CHECK_THROWS_AS(gdg::screen_blend(a, Image(2, 1, 3)), std::invalid_argument);
}

TEST_CASE("autostretch_matches_golden", "[display][golden]") {
    for (const Case& c : cases()) {
        const auto got = gdg::autostretch_u8(blend_of(c));
        const auto want = golden(c.u8).data;
        REQUIRE(got.size() == want.size());
        size_t unequal = 0;
        for (size_t i = 0; i < got.size(); ++i)
            if (static_cast<double>(got[i]) != want[i]) ++unequal;
        WARN(c.u8 << ": " << unequal << " of " << got.size() << " bytes differ");
        CHECK(unequal == 0);  // plan: exact u8
    }
    // display.autostretch literals for the branches the fields never reach (their shadows clip to
    // 0): negative and > 1 input; a tight cluster (shadows > 0, m below 1e-2); values near 1
    // (1 - shadows below 1e-2).
    auto stretch = [](std::vector<double> v) {
        Image im(static_cast<int>(v.size()), 1, 1);
        im.data = std::move(v);
        return gdg::autostretch_u8(im);
    };
    CHECK(stretch({-0.5, -0.1, 0.0, 0.05, 0.1, 0.2, 0.4, 0.8, 1.2}) ==
          std::vector<uint8_t>{0, 0, 0, 35, 64, 109, 170, 235, 255});
    CHECK(stretch({0.2, 0.2001, 0.1999, 0.20005, 0.19995, 0.2002, 0.1998, 0.9, 0.21}) ==
          std::vector<uint8_t>{60, 68, 51, 64, 56, 75, 42, 255, 217});
    CHECK(stretch({0.995, 0.996, 0.997, 0.998, 0.999, 1.0}) == std::vector<uint8_t>{23, 35, 52, 79, 130, 255});
    // A flat image: MAD 0 -> shadows = median; r = 0 -> m = 1e-3 clip; everything maps to 0.
    CHECK(gdg::autostretch_u8(Image(4, 3, 1, 0.2)) == std::vector<uint8_t>(12, 0));
    Image nan(2, 2, 1, 0.1);
    nan.data[2] = NAN;
    CHECK_THROWS_AS(gdg::autostretch_u8(nan), std::invalid_argument);
}

TEST_CASE("autostretch_split_is_identical", "[display]") {
    // autostretch_u8 == apply_stf_u8(x, stf_params(x)), byte for byte, on the golden fields (RGB
    // and mono) and on hand-made fields reaching the clip branches, at default and other settings.
    std::vector<Image> fields;
    for (const Case& c : cases()) fields.push_back(blend_of(c));
    Image lit(9, 1, 1);
    lit.data = {-0.5, -0.1, 0.0, 0.05, 0.1, 0.2, 0.4, 0.8, 1.2};
    fields.push_back(lit);
    fields.push_back(Image(4, 3, 1, 0.2));
    Image ramp(31, 17, 3);
    for (size_t i = 0; i < ramp.data.size(); ++i) ramp.data[i] = 0.01 + 0.9 * std::pow(std::sin(0.37 * i), 2.0);
    fields.push_back(ramp);
    for (const Image& f : fields) {
        for (const auto& [bg, clip] : {std::pair{0.25, -2.8}, std::pair{0.1, -1.0}, std::pair{0.5, 0.0}}) {
            const gdg::StfParams p = gdg::stf_params(f, bg, clip);
            REQUIRE(p.c0.size() == static_cast<size_t>(f.channels));
            REQUIRE(p.m.size() == static_cast<size_t>(f.channels));
            for (int c = 1; c < f.channels; ++c) {  // linked channels: one set of parameters
                CHECK(p.c0[static_cast<size_t>(c)] == p.c0[0]);
                CHECK(p.m[static_cast<size_t>(c)] == p.m[0]);
            }
            CHECK(gdg::apply_stf_u8(f, p) == gdg::autostretch_u8(f, bg, clip));
        }
    }
    // Per-channel parameters are honoured: channel 1 with m = 0.5 and c0 = 0 is the identity.
    Image two(3, 1, 2);
    two.data = {0.2, 0.2, 0.4, 0.4, 0.8, 0.8};
    const gdg::StfParams per{{0.0, 0.0}, {0.25, 0.5}};
    const auto out = gdg::apply_stf_u8(two, per);
    CHECK(out[1] == 51);   // 0.2 * 255 + 0.5
    CHECK(out[3] == 102);
    CHECK(out[5] == 204);
    CHECK(out[0] > out[1]);  // m = 0.25 brightens
    // Malformed parameters and pixels are refused.
    CHECK_THROWS_AS(gdg::apply_stf_u8(two, gdg::StfParams{{0.0}, {0.5}}), std::invalid_argument);
    CHECK_THROWS_AS(gdg::apply_stf_u8(two, gdg::StfParams{{0.0, NAN}, {0.5, 0.5}}), std::invalid_argument);
    CHECK_THROWS_AS(gdg::apply_stf_u8(two, gdg::StfParams{{0.0, 0.0}, {0.5, 1.0}}), std::invalid_argument);
    CHECK_THROWS_AS(gdg::apply_stf_u8(two, gdg::StfParams{{-0.1, 0.0}, {0.5, 0.5}}), std::invalid_argument);
    Image nan(2, 2, 1, 0.1);
    nan.data[3] = NAN;
    CHECK_THROWS_AS(gdg::apply_stf_u8(nan, gdg::StfParams{{0.0}, {0.5}}), std::invalid_argument);
    CHECK_THROWS_AS(gdg::stf_params(nan), std::invalid_argument);
    CHECK_THROWS_AS(gdg::stf_params(Image()), std::invalid_argument);
}

TEST_CASE("downscale_keeps_aspect_760", "[display]") {
    // The dialog's preview: <= 760 px wide, aspect kept (PIL: max(1, round(h * 760 / w))).
    CHECK(gdg::downscale_size(6024, 4024, 760) == std::pair<int, int>{760, 508});  // 507.67 -> 508
    CHECK(gdg::downscale_size(1520, 1010, 760) == std::pair<int, int>{760, 505});
    CHECK(gdg::downscale_size(1520, 1011, 760) == std::pair<int, int>{760, 506});  // 505.5 -> 506 (even)
    CHECK(gdg::downscale_size(1520, 1009, 760) == std::pair<int, int>{760, 504});  // 504.5 -> 504 (even)
    CHECK(gdg::downscale_size(700, 400, 760) == std::pair<int, int>{700, 400});    // never upscaled
    CHECK(gdg::downscale_size(100000, 1, 760) == std::pair<int, int>{760, 1});
    // Box average: a 4x2 RGB checker of 0 / 200 halves to 2x1 grey 100.
    std::vector<uint8_t> px;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            for (int c = 0; c < 3; ++c) px.push_back(((x + y) % 2) ? 200 : 0);
    CHECK(gdg::downscale_u8(px, 4, 2, 3, 2) == std::vector<uint8_t>(6, 100));
    CHECK(gdg::downscale_u8(px, 4, 2, 3, 760) == px);
    CHECK_THROWS_AS(gdg::downscale_u8(px, 5, 2, 3, 2), std::invalid_argument);
    // crop: numpy slice semantics (1:1 inset), clamped at the frame.
    Image im(5, 4, 1);
    for (size_t i = 0; i < im.data.size(); ++i) im.data[i] = static_cast<double>(i);
    const Image c = gdg::crop(im, 3, 2, 4, 4);
    CHECK(c.width == 2);
    CHECK(c.height == 2);
    CHECK(c.data == std::vector<double>{13, 14, 18, 19});
}
