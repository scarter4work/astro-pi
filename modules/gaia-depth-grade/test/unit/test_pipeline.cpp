// Task 11: prepare_grade / render_from_prep (pipeline.py) against the Task 1 goldens, through
// TanSky (astropy's TAN WCS of the golden header) and a fixed catalogue source.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "TanSky.h"
#include "core/HaloProfile.h"
#include "core/Match.h"
#include "core/Modulate.h"
#include "core/Pipeline.h"
#include "core/RenderStars.h"
#include "core/Transform.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using gdg::Image;
using gdg::test::golden;
using gdg::test::golden_image;
using gdg::test::TanSky;

namespace {

TanSky field_sky() {
    return TanSky::from_keywords({{"CRPIX1", 96.0},
                                  {"CRPIX2", 72.0},
                                  {"CRVAL1", 10.0},
                                  {"CRVAL2", 20.0},
                                  {"CDELT1", -0.00027777777777778},
                                  {"CDELT2", 0.00027777777777778},
                                  {"LONPOLE", 180.0}});
}

gdg::Catalog catalog(const std::string& prefix) {
    const auto ra = golden(prefix + "_ra").data, dec = golden(prefix + "_dec").data;
    const auto rm = golden(prefix + "_r_med_geo").data, rl = golden(prefix + "_r_lo_geo").data,
               rh = golden(prefix + "_r_hi_geo").data, sid = golden(prefix + "_source_id").data,
               bp = golden(prefix + "_bp_rp").data;
    gdg::Catalog c;
    for (size_t i = 0; i < ra.size(); ++i)
        c.push_back(gdg::CatalogRow{ra[i], dec[i], rm[i], rl[i], rh[i], static_cast<long long>(sid[i]), bp[i]});
    return c;
}

class FixedSource : public gdg::CatalogSource {
public:
    explicit FixedSource(gdg::Catalog c) : cat_(std::move(c)) {}
    gdg::Catalog distances_for(const gdg::Footprint& fp) override {
        ++calls;
        seen = fp;
        return cat_;
    }
    int calls = 0;
    gdg::Footprint seen;

private:
    gdg::Catalog cat_;
};

struct Captured {
    std::vector<std::string> info, warning;
    gdg::Log log() {
        return gdg::Log{[this](const std::string& m) { info.push_back(m); },
                        [this](const std::string& m) { warning.push_back(m); }};
    }
};

bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

void check_prepare(const std::string& p, const char* stars_name, double median_offset) {
    const Image stars = golden_image(stars_name);
    FixedSource src(catalog(p + "catalog"));
    Captured cap;
    const gdg::PrepResult prep = gdg::prepare_grade(stars, field_sky(), gdg::GradeConfig{}, src, cap.log());
    CHECK(src.calls == 1);
    const auto fp = golden(p + "footprint").data;
    CHECK(std::fabs(src.seen.center_ra - fp[0]) <= 1e-10);
    CHECK(std::fabs(src.seen.center_dec - fp[1]) <= 1e-10);
    CHECK(std::fabs(src.seen.radius_deg - fp[2]) <= 1e-10);
    CHECK(prep.footprint.radius_deg == src.seen.radius_deg);

    const auto mx = golden(p + "match_table_x").data, mm = golden(p + "match_table_matched").data,
               rm = golden(p + "match_table_r_med_geo").data, rl = golden(p + "match_table_r_lo_geo").data,
               rh = golden(p + "match_table_r_hi_geo").data, bp = golden(p + "match_table_bp_rp").data;
    REQUIRE(prep.table.size() == mx.size());
    size_t bad = 0;
    for (size_t i = 0; i < mx.size(); ++i) {
        const auto& r = prep.table[i];
        if (std::fabs(r.x - mx[i]) > 1e-9 || r.matched != (mm[i] != 0.0) || !same(r.r_med, rm[i]) || !same(r.r_lo, rl[i]) ||
            !same(r.r_hi, rh[i]) || !same(r.bp_rp, bp[i]))
            ++bad;
    }
    CHECK(bad == 0);
    CHECK(prep.qa.n_detected == 37);
    CHECK(prep.qa.n_matched == 26);
    CHECK(prep.qa.match_rate == 0.7027027027027027);
    // Catalogue positions come from TanSky here (astropy in the golden): |delta| ~1e-13 px.
    WARN(p << "median offset |delta| " << std::fabs(prep.qa.median_offset_px - median_offset) << " px");
    CHECK(std::fabs(prep.qa.median_offset_px - median_offset) <= 1e-9);
    CHECK(!prep.qa.low_match_warning);
    CHECK(cap.warning.empty());
}

void check_f32(const Image& got, const std::string& name) {
    const Image want = golden_image(name);
    REQUIRE(got.same_shape(want));
    size_t f32_unequal = 0;
    double d = 0.0;
    for (size_t i = 0; i < got.data.size(); ++i) {
        d = std::max(d, std::fabs(got.data[i] - want.data[i]));
        if (static_cast<double>(static_cast<float>(got.data[i])) != want.data[i]) ++f32_unequal;
    }
    WARN(name << ": max |delta| " << d << ", " << f32_unequal << " samples not float32-identical");
    CHECK(f32_unequal == 0);
}

// The golden match table as a PrepResult (render_from_prep's input in the generator).
gdg::PrepResult golden_prep(const std::string& p) {
    const auto x = golden(p + "match_table_x").data, y = golden(p + "match_table_y").data,
               f = golden(p + "match_table_flux").data, mm = golden(p + "match_table_matched").data,
               rm = golden(p + "match_table_r_med_geo").data, rl = golden(p + "match_table_r_lo_geo").data,
               rh = golden(p + "match_table_r_hi_geo").data, bp = golden(p + "match_table_bp_rp").data;
    gdg::PrepResult prep;
    for (size_t i = 0; i < x.size(); ++i) {
        gdg::StarRow r{x[i], y[i], f[i], rm[i], rl[i], rh[i], bp[i], mm[i] != 0.0};
        prep.table.push_back(r);
    }
    return prep;
}

}  // namespace

TEST_CASE("prepare_matches_golden_qa", "[pipeline][golden]") {
    check_prepare("", "stars_rgb", 0.9221258377147912);
    check_prepare("mono_", "mono_stars", 0.9221258851340097);
}

TEST_CASE("prepare_low_match_warns", "[pipeline][golden]") {
    FixedSource src(catalog("catalog"));
    Captured cap;
    gdg::GradeConfig cfg;
    cfg.min_match_rate = 0.71;
    const gdg::PrepResult prep = gdg::prepare_grade(golden_image("stars_rgb"), field_sky(), cfg, src, cap.log());
    CHECK(prep.qa.low_match_warning);
    REQUIRE(cap.warning.size() == 1);
    CHECK(cap.warning[0] == "LOW MATCH RATE 0.70 < 0.71 \xE2\x80\x94 depth grade is unreliable");
    // At exactly the rate: not low (strict <).
    cfg.min_match_rate = 0.7027027027027027;
    Captured cap2;
    CHECK(!gdg::prepare_grade(golden_image("stars_rgb"), field_sky(), cfg, src, cap2.log()).qa.low_match_warning);
    CHECK(cap2.warning.empty());
}

TEST_CASE("render_from_prep_matches_golden", "[pipeline][golden]") {
    // Colour with the dialog gains, mono with the engine defaults (the generator's two runs).
    gdg::GradeConfig dialog;
    dialog.gains = gdg::Gains{0.65, 0.85, 0.12, 0.4, 1.0};
    Captured cap;
    check_f32(gdg::render_from_prep(golden_image("stars_rgb"), golden_prep(""), dialog, cap.log()), "rendered_stars");
    check_f32(gdg::render_from_prep(golden_image("mono_stars"), golden_prep("mono_"), gdg::GradeConfig{}, cap.log()),
              "mono_rendered_stars");
    CHECK(cap.warning.empty());  // both fixtures have an empirical halo
}

TEST_CASE("prepare_then_render_is_the_product_path", "[pipeline][golden]") {
    // prepare_grade's own table feeds render_from_prep to the same pixels.
    FixedSource src(catalog("catalog"));
    gdg::GradeConfig dialog;
    dialog.gains = gdg::Gains{0.65, 0.85, 0.12, 0.4, 1.0};
    const Image stars = golden_image("stars_rgb");
    const gdg::PrepResult prep = gdg::prepare_grade(stars, field_sky(), dialog, src, gdg::Log{});
    check_f32(gdg::render_from_prep(stars, prep, dialog, gdg::Log{}), "rendered_stars");
}

TEST_CASE("pipeline_threads_every_config_field", "[pipeline]") {
    // Non-default p_low/p_high/neutral/base sigma/halo gain/gains reach the render, and
    // fwhm/threshold/tolerance reach detection and matching (compared with the stages called
    // directly with the same values).
    gdg::GradeConfig cfg;
    cfg.gains = gdg::Gains{0.9, 1.3, 0.2, 0.7, 1.7};
    cfg.p_low = 12.0;
    cfg.p_high = 81.0;
    cfg.neutral_strength = 0.25;
    cfg.base_sigma_px = 1.6;
    cfg.halo_min_refs = 4;
    cfg.detect_fwhm = 2.5;
    cfg.detect_threshold_sigma = 9.0;
    cfg.match_tolerance_px = 1.5;
    const Image stars = golden_image("stars_rgb");
    FixedSource src(catalog("catalog"));
    const gdg::PrepResult prep = gdg::prepare_grade(stars, field_sky(), cfg, src, gdg::Log{});

    const auto det = gdg::detect_stars(gdg::luminance(stars), 2.5, 9.0);
    gdg::MatchStats st;
    const auto table = gdg::cross_match(det, catalog("catalog"), field_sky(), 1.5, st);
    REQUIRE(prep.table.size() == table.size());
    CHECK(prep.qa.n_matched == st.n_matched);
    CHECK(prep.qa.n_detected != 37);  // the non-default detection settings took effect
    CHECK(prep.qa.median_offset_px == st.median_offset_px);

    std::vector<double> rm, rl, rh;
    for (const auto& r : prep.table) {
        rm.push_back(r.r_med);
        rl.push_back(r.r_lo);
        rh.push_back(r.r_hi);
    }
    const auto eff = gdg::effective_strength(rm, rl, rh, 12.0, 81.0, 0.25);
    const auto mod = gdg::compute_modulation(eff, cfg.gains);
    const auto halo = gdg::build_halo_profile(stars, prep.table, 1.6, cfg, gdg::Log{});
    const Image want = gdg::render_stars(stars, prep.table, mod, 1.6, &halo, 1.7);
    CHECK(gdg::render_from_prep(stars, prep, cfg, gdg::Log{}).data == want.data);
}
