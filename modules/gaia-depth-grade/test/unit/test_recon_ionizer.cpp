// Task 18 (recon plan task 5): ionizer.py against the Task 1 golden cases (stage.json
// "recon_pick_ionizer"), plus the edge cases of gaia-depth-grade/tests/recon/test_ionizer.py.
#include <catch_amalgamated.hpp>

#include "ReconGolden.h"
#include "core/recon/Ionizer.h"

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace gdg::recon;
using gdg::test::golden;

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

void check_ionizer(const Ionizer& got, double x, double y, std::optional<double> d, const std::string& source) {
    CHECK(got.x == x);
    CHECK(got.y == y);
    CHECK(got.distance_pc.has_value() == d.has_value());
    if (got.distance_pc && d) CHECK(*got.distance_pc == *d);
    CHECK(got.source == source);
}

IonizerTable golden_table() {
    IonizerTable t;
    t.x = golden("recon_ionizer_table_x").data;
    t.y = golden("recon_ionizer_table_y").data;
    t.flux = golden("recon_ionizer_table_flux").data;
    t.bp_rp = golden("recon_ionizer_table_bp_rp").data;
    t.r_med_geo = golden("recon_ionizer_table_r_med_geo").data;
    return t;
}

struct Row {
    double x, y, flux, bp_rp;
    std::optional<double> r_med;
};

/// test_ionizer._table: x, y, flux, r_med_geo (NaN when unmatched) and optionally bp_rp.
IonizerTable table(const std::vector<Row>& rows, bool with_bp_rp = true) {
    IonizerTable t;
    t.r_med_geo.emplace();
    if (with_bp_rp) t.bp_rp.emplace();
    for (const Row& r : rows) {
        t.x.push_back(r.x);
        t.y.push_back(r.y);
        t.flux.push_back(r.flux);
        t.r_med_geo->push_back(r.r_med ? *r.r_med : NaN);
        if (with_bp_rp) t.bp_rp->push_back(r.bp_rp);
    }
    return t;
}

constexpr int NY = 100, NX = 200;  // test_ionizer.SHAPE

}  // namespace

TEST_CASE("pick_ionizer_matches_golden", "[recon][ionizer][golden]") {
    const int n = 140;  // stage.json shape_hw [140, 140]
    const double gx = 59.07863826198037, gy = 69.29988179267875, gd = 574.2669529853504;
    const IonizerTable tab = golden_table();
    check_ionizer(pick_ionizer(&tab, n, n), gx, gy, gd, "gaia-blue");

    IonizerTable nocol = tab;
    nocol.bp_rp.reset();
    check_ionizer(pick_ionizer(&nocol, n, n), gx, gy, gd, "gaia-bright");

    IonizerTable allnan = tab;
    for (double& v : *allnan.bp_rp) v = NaN;
    check_ionizer(pick_ionizer(&allnan, n, n), gx, gy, gd, "gaia-bright");

    const IonizerTable empty = table({});
    check_ionizer(pick_ionizer(&empty, n, n), 70.0, 70.0, std::nullopt, "field-center");
    check_ionizer(pick_ionizer(nullptr, n, n), 70.0, 70.0, std::nullopt, "field-center");

    IonizerTable outside = tab;
    for (double& v : outside.x) v = -5.0;
    check_ionizer(pick_ionizer(&outside, n, n), 70.0, 70.0, std::nullopt, "field-center");

    IonizerTable nofluxpos = tab;
    for (double& v : nofluxpos.flux) v = -1.0;
    check_ionizer(pick_ionizer(&nofluxpos, n, n), 70.0, 70.0, std::nullopt, "field-center");

    check_ionizer(ionizer_from_xy(33.5, 44.25, 812.0), 33.5, 44.25, 812.0, "override");
    check_ionizer(ionizer_from_xy(1, 2, std::nullopt), 1.0, 2.0, std::nullopt, "override");
    CHECK(IONIZER_FLUX_WEIGHT == 0.4);
}

TEST_CASE("picks_blue_luminous_star", "[recon][ionizer]") {
    const IonizerTable t = table({{10, 10, 1000.0, 2.5, 500.0},    // red bright
                                  {50, 40, 800.0, -0.2, 1200.0},   // blue bright
                                  {90, 60, 50.0, -0.3, 900.0}});   // blue faint
    check_ionizer(pick_ionizer(&t, NY, NX), 50.0, 40.0, 1200.0, "gaia-blue");
}

TEST_CASE("falls_back_to_brightest_without_colour", "[recon][ionizer]") {
    const IonizerTable t = table({{10, 10, 100.0, NaN, 500.0}, {20, 30, 900.0, NaN, 700.0}});
    check_ionizer(pick_ionizer(&t, NY, NX), 20.0, 30.0, 700.0, "gaia-bright");
}

TEST_CASE("falls_back_when_bp_rp_column_absent", "[recon][ionizer]") {
    const IonizerTable t = table({{10, 10, 100.0, 0.0, 500.0}, {20, 30, 900.0, 0.0, 700.0}}, false);
    REQUIRE_FALSE(t.bp_rp);
    check_ionizer(pick_ionizer(&t, NY, NX), 20.0, 30.0, 700.0, "gaia-bright");
}

TEST_CASE("unmatched_pick_has_no_distance", "[recon][ionizer]") {
    const IonizerTable t = table({{20, 30, 900.0, NaN, std::nullopt}, {10, 10, 100.0, NaN, 500.0}});
    CHECK_FALSE(pick_ionizer(&t, NY, NX).distance_pc);
    // and with no r_med_geo column at all
    IonizerTable nod = t;
    nod.r_med_geo.reset();
    CHECK_FALSE(pick_ionizer(&nod, NY, NX).distance_pc);
}

TEST_CASE("stars_outside_image_bounds_ignored", "[recon][ionizer]") {
    const IonizerTable t = table({{500, 10, 9999.0, -0.5, 100.0}, {20, 30, 10.0, 0.5, 700.0}});
    check_ionizer(pick_ionizer(&t, NY, NX), 20.0, 30.0, 700.0, "gaia-blue");
    // bounds are half-open: x == nx and y == ny are outside, 0 is inside
    const IonizerTable edge = table({{200, 10, 9999.0, -0.5, 100.0}, {10, 100, 9999.0, -0.5, 100.0},
                                     {0, 0, 1.0, 0.5, 700.0}});
    check_ionizer(pick_ionizer(&edge, NY, NX), 0.0, 0.0, 700.0, "gaia-blue");
}

TEST_CASE("empty_table_returns_field_center", "[recon][ionizer]") {
    const IonizerTable t = table({});
    check_ionizer(pick_ionizer(&t, NY, NX), 100.0, 50.0, std::nullopt, "field-center");
}

TEST_CASE("override_returns_specified_point", "[recon][ionizer]") {
    check_ionizer(ionizer_from_xy(12.5, 7.0, 1500.0), 12.5, 7.0, 1500.0, "override");
}

TEST_CASE("score_trades_colour_against_flux", "[recon][ionizer]") {
    // score = bp_rp - 0.4 log10(flux / fmax). 10x fainter costs +0.4 mag.
    const IonizerTable a = table({{10, 10, 1000.0, 0.5, 1.0}, {20, 20, 100.0, 0.05, 2.0}});  // 0.5 vs 0.45
    CHECK(pick_ionizer(&a, NY, NX).x == 20.0);
    const IonizerTable b = table({{10, 10, 1000.0, 0.5, 1.0}, {20, 20, 100.0, 0.15, 2.0}});  // 0.5 vs 0.55
    CHECK(pick_ionizer(&b, NY, NX).x == 10.0);
    // ties keep the first row (np.argmin / np.argmax)
    const IonizerTable tie = table({{10, 10, 100.0, 0.3, 1.0}, {20, 20, 100.0, 0.3, 2.0}});
    CHECK(pick_ionizer(&tie, NY, NX).x == 10.0);
    IonizerTable tie_nc = tie;
    tie_nc.bp_rp.reset();
    CHECK(pick_ionizer(&tie_nc, NY, NX).x == 10.0);
    // a brighter colourless star is never picked while any usable star has a colour
    const IonizerTable c = table({{30, 30, 1e6, NaN, 1.0}, {10, 10, 1000.0, 0.5, 1.0}, {20, 20, 100.0, 0.05, 2.0}});
    const Ionizer ic = pick_ionizer(&c, NY, NX);
    CHECK(ic.x == 20.0);
    CHECK(ic.source == "gaia-blue");
}

TEST_CASE("unusable_rows_are_skipped", "[recon][ionizer]") {
    // non-finite position or flux, zero flux: skipped, in both the colour and the bright path
    const IonizerTable t = table({{NaN, 10, 1e6, -1.0, 1.0}, {10, NaN, 1e6, -1.0, 1.0}, {10, 10, NaN, -1.0, 1.0},
                                  {10, 10, INFINITY, -1.0, 1.0}, {10, 10, 0.0, -1.0, 1.0}, {42, 43, 5.0, 1.0, 9.0}});
    check_ionizer(pick_ionizer(&t, NY, NX), 42.0, 43.0, 9.0, "gaia-blue");
    IonizerTable nc = t;
    nc.bp_rp.reset();
    check_ionizer(pick_ionizer(&nc, NY, NX), 42.0, 43.0, 9.0, "gaia-bright");
    // zero flux alone is unusable (Python oracle: field-center)
    const IonizerTable zero = table({{10, 10, 0.0, NaN, 5.0}});
    check_ionizer(pick_ionizer(&zero, NY, NX), 100.0, 50.0, std::nullopt, "field-center");
    // an infinite colour is not "finite colour": falls back to brightness
    const IonizerTable inf_col = table({{10, 10, 5.0, INFINITY, 1.0}, {20, 20, 50.0, NaN, 2.0}});
    check_ionizer(pick_ionizer(&inf_col, NY, NX), 20.0, 20.0, 2.0, "gaia-bright");
}

TEST_CASE("ionizer_table_from_star_rows", "[recon][ionizer]") {
    std::vector<gdg::StarRow> rows(2);
    rows[0] = gdg::StarRow{1.0, 2.0, 3.0};
    rows[1] = gdg::StarRow{4.0, 5.0, 6.0};
    rows[1].r_med = 700.0;
    rows[1].bp_rp = -0.2;
    const IonizerTable t = ionizer_table(rows);
    CHECK(t.x == std::vector<double>{1.0, 4.0});
    CHECK(t.y == std::vector<double>{2.0, 5.0});
    CHECK(t.flux == std::vector<double>{3.0, 6.0});
    REQUIRE(t.bp_rp);
    REQUIRE(t.r_med_geo);
    CHECK(std::isnan((*t.bp_rp)[0]));
    CHECK((*t.bp_rp)[1] == -0.2);
    CHECK((*t.r_med_geo)[1] == 700.0);
    check_ionizer(pick_ionizer(&t, NY, NX), 4.0, 5.0, 700.0, "gaia-blue");
}

TEST_CASE("ragged_table_throws", "[recon][ionizer]") {
    IonizerTable t = table({{10, 10, 1.0, 0.1, 1.0}});
    t.y.push_back(3.0);
    CHECK_THROWS_AS(pick_ionizer(&t, NY, NX), std::invalid_argument);
    IonizerTable u = table({{10, 10, 1.0, 0.1, 1.0}});
    u.bp_rp->push_back(0.0);
    CHECK_THROWS_AS(pick_ionizer(&u, NY, NX), std::invalid_argument);
}
