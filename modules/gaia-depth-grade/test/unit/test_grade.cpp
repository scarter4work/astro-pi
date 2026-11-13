// Tasks 16-17: the process' engine settings (defaults = the dialog's, every field reaching the
// engine), the recombine (= the sidecar's render + nebula + screen blend, against the Task 1
// goldens), the preview pixels and inset geometry of the dialog, and the QA status text.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/Display.h"
#include "core/Grade.h"
#include "core/Nebula.h"
#include "core/Pipeline.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using gdg::Image;
using gdg::test::golden;
using gdg::test::golden_image;

namespace {

// The golden match table as a PrepResult (as test_pipeline's golden_prep).
gdg::PrepResult golden_prep(const std::string& p) {
    const auto x = golden(p + "match_table_x").data, y = golden(p + "match_table_y").data,
               f = golden(p + "match_table_flux").data, mm = golden(p + "match_table_matched").data,
               rm = golden(p + "match_table_r_med_geo").data, rl = golden(p + "match_table_r_lo_geo").data,
               rh = golden(p + "match_table_r_hi_geo").data, bp = golden(p + "match_table_bp_rp").data;
    gdg::PrepResult prep;
    for (size_t i = 0; i < x.size(); ++i) prep.table.push_back(gdg::StarRow{x[i], y[i], f[i], rm[i], rl[i], rh[i], bp[i], mm[i] != 0.0});
    return prep;
}

size_t f32_unequal(const Image& got, const Image& want) {
    REQUIRE(got.same_shape(want));
    size_t n = 0;
    for (size_t i = 0; i < got.data.size(); ++i)
        if (static_cast<double>(static_cast<float>(got.data[i])) != want.data[i]) ++n;
    return n;
}

}  // namespace

TEST_CASE("settings_defaults_are_the_dialogs", "[grade]") {
    // GaiaDepthGradeDialog.js (F12-F25) and config.py for the engine-only fields (F43).
    const gdg::GradeSettings s;
    CHECK(s.brightness == 0.65);
    CHECK(s.size == 0.85);
    CHECK(s.contrast == 0.12);
    CHECK(s.saturation == 0.4);
    CHECK(s.halo_sigma == 2.0);
    CHECK(s.p_low == 5.0);
    CHECK(s.p_high == 95.0);
    CHECK(!s.nebula_enabled);
    CHECK(s.nebula_strength == 1.0);
    CHECK(s.nebula_atmos == 1.0);
    CHECK(s.nebula_structure == 1.0);
    CHECK(s.detect_sigma == 6.0);
    CHECK(s.match_tolerance == 4.0);
    CHECK(!s.online);  // the Offline box is checked
    CHECK(s.mag_limit_enabled);
    CHECK(s.mag_limit == 18.0);
    CHECK(s.halo_gain == 1.0);
    CHECK(s.detect_fwhm == 3.0);
    CHECK(s.min_match_rate == 0.3);
    CHECK(s.neutral_strength == 0.0);
    CHECK(s.parallax_snr == 5.0);
    CHECK(s.halo_min_refs == 8);
    CHECK(s.halo_radius_sigma == 8.0);
    CHECK(s.halo_moffat_beta == 2.5);
    // ... and those engine-only fields equal config.py's GradeConfig defaults.
    const gdg::GradeConfig e;
    const gdg::GradeConfig c = gdg::grade_config(s);
    CHECK(c.detect_fwhm == e.detect_fwhm);
    CHECK(c.min_match_rate == e.min_match_rate);
    CHECK(c.neutral_strength == e.neutral_strength);
    CHECK(c.gaia_parallax_snr == e.gaia_parallax_snr);
    CHECK(c.halo_min_refs == e.halo_min_refs);
    CHECK(c.halo_radius_sigma == e.halo_radius_sigma);
    CHECK(c.halo_moffat_beta == e.halo_moffat_beta);
    CHECK(c.gains.halo_gain == e.gains.halo_gain);
    CHECK(c.gaia_mag_limit == e.gaia_mag_limit);
}

TEST_CASE("params_reach_core", "[grade]") {
    // Every field set to a distinct non-default value lands in its GradeConfig field (F43).
    gdg::GradeSettings s;
    s.brightness = 1.11;
    s.size = 1.22;
    s.contrast = 0.33;
    s.saturation = 0.44;
    s.halo_gain = 2.5;
    s.halo_sigma = 3.3;
    s.p_low = 7.5;
    s.p_high = 88.5;
    s.neutral_strength = 0.15;
    s.halo_min_refs = 5;
    s.halo_radius_sigma = 6.5;
    s.halo_moffat_beta = 3.25;
    s.detect_sigma = 9.5;
    s.detect_fwhm = 2.25;
    s.match_tolerance = 2.75;
    s.min_match_rate = 0.45;
    s.mag_limit = 16.5;
    s.parallax_snr = 7.0;
    const gdg::GradeConfig c = gdg::grade_config(s);
    CHECK(c.gains.brightness == 1.11);
    CHECK(c.gains.size == 1.22);
    CHECK(c.gains.contrast == 0.33);
    CHECK(c.gains.saturation == 0.44);
    CHECK(c.gains.halo_gain == 2.5);
    CHECK(c.base_sigma_px == 3.3);
    CHECK(c.p_low == 7.5);
    CHECK(c.p_high == 88.5);
    CHECK(c.neutral_strength == 0.15);
    CHECK(c.halo_min_refs == 5);
    CHECK(c.halo_radius_sigma == 6.5);
    CHECK(c.halo_moffat_beta == 3.25);
    CHECK(c.detect_threshold_sigma == 9.5);
    CHECK(c.detect_fwhm == 2.25);
    CHECK(c.match_tolerance_px == 2.75);
    CHECK(c.min_match_rate == 0.45);
    REQUIRE(c.gaia_mag_limit.has_value());
    CHECK(*c.gaia_mag_limit == 16.5);
    CHECK(c.gaia_parallax_snr == 7.0);
    // The cut switched off is Python's gaia_mag_limit = None.
    s.mag_limit_enabled = false;
    CHECK(!gdg::grade_config(s).gaia_mag_limit.has_value());
}

TEST_CASE("nebula_active_needs_enabled_and_positive_strength", "[grade]") {
    gdg::GradeSettings s;
    CHECK(!gdg::nebula_active(s));
    s.nebula_enabled = true;
    CHECK(gdg::nebula_active(s));
    s.nebula_strength = 0.0;
    CHECK(!gdg::nebula_active(s));
}

TEST_CASE("prepare_staleness_follows_prepare_fields_only", "[grade]") {
    const gdg::GradeSettings a;
    gdg::GradeSettings b = a;
    b.brightness = 1.5;
    b.size = 0.1;
    b.halo_sigma = 4;
    b.p_low = 1;
    b.nebula_enabled = true;
    b.halo_gain = 3;
    b.neutral_strength = 0.5;
    b.halo_min_refs = 3;
    CHECK(!gdg::prepare_settings_differ(a, b));  // all live (render) fields
    for (int k = 0; k < 7; ++k) {
        gdg::GradeSettings c = a;
        switch (k) {
            case 0: c.detect_sigma = 7; break;
            case 1: c.match_tolerance = 5; break;
            case 2: c.online = true; break;
            case 3: c.mag_limit = 17; break;
            case 4: c.detect_fwhm = 2; break;
            case 5: c.min_match_rate = 0.5; break;
            case 6: c.parallax_snr = 3; break;
        }
        CHECK(gdg::prepare_settings_differ(a, c));
    }
    gdg::GradeSettings off1 = a, off2 = a;
    off1.mag_limit_enabled = off2.mag_limit_enabled = false;
    off2.mag_limit = 12;  // the limit's value does not matter while the cut is off
    CHECK(!gdg::prepare_settings_differ(off1, off2));
}

TEST_CASE("fixed_catalog_source_only_answers_its_footprint", "[grade]") {
    gdg::Catalog cat(3);
    cat[1].source_id = 42;
    gdg::FixedCatalogSource src(gdg::Footprint{10.0, 20.0, 0.5}, cat);
    const gdg::Catalog got = src.distances_for(gdg::Footprint{10.0, 20.0, 0.5});
    REQUIRE(got.size() == 3);
    CHECK(got[1].source_id == 42);
    CHECK_THROWS_AS(src.distances_for(gdg::Footprint{10.0, 20.0, 0.50000001}), std::logic_error);
}

TEST_CASE("composite_matches_golden_screen_blend", "[grade][golden]") {
    // The generator: screen_blend(render_from_prep(stars, dialog gains), render_nebula(starless,
    // strength 1, atmos 1, structure 1)) -- the sidecar's preview / Execute image.
    gdg::GradeSettings s;  // the dialog defaults
    s.nebula_enabled = true;
    const gdg::PrepResult prep = golden_prep("");
    const Image stars = golden_image("stars_rgb"), starless = golden_image("starless_rgb");
    const Image comp = gdg::grade_composite(stars, starless, prep, s, gdg::Log{});
    CHECK(f32_unequal(comp, golden_image("screen_blend")) == 0);
    // The preview stretch of it (exact u8).
    const auto u8 = gdg::autostretch_u8(comp);
    const auto want = golden("autostretch_u8").data;
    REQUIRE(u8.size() == want.size());
    size_t bad = 0;
    for (size_t i = 0; i < u8.size(); ++i) bad += static_cast<double>(u8[i]) != want[i];
    CHECK(bad == 0);
}

TEST_CASE("composite_mono_matches_golden", "[grade][golden]") {
    // Mono (Review Focus 2): engine-default gains, nebula (2, 0.5, 1.5): mono_screen_blend.
    gdg::GradeSettings s;
    s.brightness = 0.5;
    s.size = 0.4;
    s.contrast = 0.3;
    s.saturation = 0.3;
    s.nebula_enabled = true;
    s.nebula_strength = 2.0;
    s.nebula_atmos = 0.5;
    s.nebula_structure = 1.5;
    const Image comp = gdg::grade_composite(golden_image("mono_stars"), golden_image("mono_starless"),
                                            golden_prep("mono_"), s, gdg::Log{});
    CHECK(comp.channels == 1);
    CHECK(f32_unequal(comp, golden_image("mono_screen_blend")) == 0);
}

TEST_CASE("composite_without_nebula_blends_the_raw_starless", "[grade][golden]") {
    gdg::GradeSettings s;  // nebula off
    const gdg::PrepResult prep = golden_prep("");
    const Image stars = golden_image("stars_rgb"), starless = golden_image("starless_rgb");
    const Image comp = gdg::grade_composite(stars, starless, prep, s, gdg::Log{});
    const Image want = gdg::screen_blend(gdg::render_from_prep(stars, prep, gdg::grade_config(s), gdg::Log{}), starless);
    CHECK(comp.data == want.data);
    // Enabled at strength 0: the same (the dialog adds no nebula arguments then).
    s.nebula_enabled = true;
    s.nebula_strength = 0.0;
    CHECK(gdg::grade_composite(stars, starless, prep, s, gdg::Log{}).data == want.data);
    // Shapes must agree.
    CHECK_THROWS_AS(gdg::grade_composite(stars, golden_image("mono_starless"), prep, s, gdg::Log{}),
                    std::invalid_argument);
}

TEST_CASE("inset_geometry_matches_the_dialog", "[grade]") {
    int cx = 0, cy = 0;
    gdg::default_inset_center(3840, 2160, cx, cy);
    CHECK(cx == 1920);
    CHECK(cy == 1080);
    gdg::default_inset_center(513, 385, cx, cy);  // Math.round(256.5) = 257
    CHECK(cx == 257);
    CHECK(cy == 193);
    // Centre: x = cx - 128.
    gdg::InsetRegion r = gdg::inset_region(3840, 2160, 1920, 1080);
    CHECK(r.x == 1792);
    CHECK(r.y == 952);
    CHECK(r.w == 256);
    CHECK(r.h == 256);
    // Clamped at both ends.
    r = gdg::inset_region(3840, 2160, 10, 2150);
    CHECK(r.x == 0);
    CHECK(r.y == 2160 - 256);
    r = gdg::inset_region(3840, 2160, 3839, 5);
    CHECK(r.x == 3840 - 256);
    CHECK(r.y == 0);
    // An image narrower than the inset starts at 0.
    r = gdg::inset_region(64, 48, 32, 24);
    CHECK(r.x == 0);
    CHECK(r.y == 0);
    // A click on a 760-wide preview of a 3840-wide image: scale 3840/760.
    gdg::inset_center_from_click(380, 100, 3840, 760, cx, cy);
    CHECK(cx == 1920);
    CHECK(cy == 505);  // Math.round(505.26)
    gdg::inset_center_from_click(0, 0, 3840, 760, cx, cy);
    CHECK(cx == 0);
    CHECK(cy == 0);
}

TEST_CASE("preview_pixels_full_and_inset", "[grade][golden]") {
    gdg::GradeSettings s;
    s.nebula_enabled = true;
    const Image comp = gdg::grade_composite(golden_image("stars_rgb"), golden_image("starless_rgb"), golden_prep(""), s,
                                            gdg::Log{});
    // 192 x 144 -> 100 wide: 75 rows (round(144 * 100/192) = 75).
    REQUIRE(comp.width == 192);
    REQUIRE(comp.height == 144);
    const gdg::InsetRegion r = gdg::inset_region(comp.width, comp.height, 100, 50, 64);
    const gdg::PreviewPixels p = gdg::preview_pixels(comp, 100, r);
    CHECK(p.width == 100);
    CHECK(p.height == 75);
    CHECK(p.channels == 3);
    CHECK(p.full.size() == size_t(100) * 75 * 3);
    CHECK(p.full == gdg::downscale_u8(gdg::autostretch_u8(comp), comp.width, comp.height, 3, 100));
    // The inset is cut 1:1 from the full-resolution stretch.
    REQUIRE(p.inset_width == 64);
    REQUIRE(p.inset_height == 64);
    const auto u8 = gdg::autostretch_u8(comp);
    size_t bad = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            for (int c = 0; c < 3; ++c)
                bad += p.inset[(size_t(y) * 64 + x) * 3 + c] != u8[((size_t(r.y + y)) * comp.width + r.x + x) * 3 + c];
    CHECK(bad == 0);
    // No downscale below max_width; an inset past the far edge is clipped (numpy slicing).
    const gdg::PreviewPixels q = gdg::preview_pixels(comp, 760, gdg::InsetRegion{180, 140, 256, 256});
    CHECK(q.width == 192);
    CHECK(q.height == 144);
    CHECK(q.inset_width == 12);
    CHECK(q.inset_height == 4);
}

TEST_CASE("preview_pixels_fit_the_control", "[grade]") {
    // With a height bound the full view fits max_width x max_height (fit_size); the inset is
    // unchanged; without one (0) it is the sidecar's width-only downscale.
    gdg::GradeSettings s;
    const Image comp = gdg::grade_composite(golden_image("stars_rgb"), golden_image("starless_rgb"), golden_prep(""), s,
                                            gdg::Log{});
    const gdg::InsetRegion r = gdg::inset_region(comp.width, comp.height, 100, 50, 64);
    const gdg::PreviewPixels a = gdg::preview_pixels(comp, 100, 60, r);  // 192 x 144 -> 80 x 60 (height-bound)
    CHECK(a.width == 80);
    CHECK(a.height == 60);
    CHECK(a.full == gdg::downscale_u8_to(gdg::autostretch_u8(comp), comp.width, comp.height, 3, 80, 60));
    const gdg::PreviewPixels w = gdg::preview_pixels(comp, 100, r);
    CHECK(a.inset == w.inset);
    const gdg::PreviewPixels b = gdg::preview_pixels(comp, 100, 0, r);
    CHECK(b.width == w.width);
    CHECK(b.height == w.height);
    CHECK(b.full == w.full);
}

TEST_CASE("qa_summary_text_is_the_dialogs", "[grade]") {
    gdg::QA qa{120, 87, 0.725, 0.94, false};
    CHECK(gdg::qa_summary(qa) == "Prepared: 87/120 matched (rate 0.72, offset 0.9 px)");
    qa.low_match_warning = true;
    CHECK(gdg::qa_summary(qa) ==
          "Prepared: 87/120 matched (rate 0.72, offset 0.9 px)  \xE2\x9A\xA0 LOW MATCH \xE2\x80\x94 depth grade unreliable");
}

TEST_CASE("honesty_tag_is_cli_HONESTY", "[grade]") {
    CHECK(std::string(gdg::kHonesty) == "GAIA depth-derived; physically motivated, not per-pixel correct for gas");
}
