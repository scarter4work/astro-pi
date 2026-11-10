// Task 12: offline distances (local_gaia.py) against the Task 1 goldens, and the settings-file
// discovery of the Gaia database files (PJSR discoverGaiaDbPaths).
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "core/Distances.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using gdg::test::golden;

namespace {

struct Raw {
    std::vector<double> ra, dec, plx, g, bp;
};

// catalog_raw: (47, 5) columns ra, dec, parallax_mas, G, bp_rp (stage.json).
Raw raw() {
    const auto a = golden("catalog_raw");
    REQUIRE(a.shape.size() == 2);
    REQUIRE(a.shape[1] == 5);
    Raw r;
    for (size_t i = 0; i < a.shape[0]; ++i) {
        r.ra.push_back(a.data[5 * i]);
        r.dec.push_back(a.data[5 * i + 1]);
        r.plx.push_back(a.data[5 * i + 2]);
        r.g.push_back(a.data[5 * i + 3]);
        r.bp.push_back(a.data[5 * i + 4]);
    }
    return r;
}

struct Captured {
    std::vector<std::string> info, warning;
    gdg::Log log() {
        return gdg::Log{[this](const std::string& m) { info.push_back(m); },
                        [this](const std::string& m) { warning.push_back(m); }};
    }
};

bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

void check_catalog(const gdg::Catalog& c, const std::string& p) {
    const auto ra = golden(p + "_ra").data, dec = golden(p + "_dec").data, rm = golden(p + "_r_med_geo").data,
               rl = golden(p + "_r_lo_geo").data, rh = golden(p + "_r_hi_geo").data,
               sid = golden(p + "_source_id").data, bp = golden(p + "_bp_rp").data;
    REQUIRE(c.size() == ra.size());
    size_t bad = 0;
    for (size_t i = 0; i < c.size(); ++i)
        if (c[i].ra != ra[i] || c[i].dec != dec[i] || c[i].r_med != rm[i] || c[i].r_lo != rl[i] ||
            c[i].r_hi != rh[i] || static_cast<double>(c[i].source_id) != sid[i] || !same(c[i].bp_rp, bp[i]))
            ++bad;
    CHECK(bad == 0);
}

}  // namespace

TEST_CASE("knot_sigma_matches", "[distances][golden]") {
    const Raw r = raw();
    const auto want = golden("sigma_parallax").data;
    const auto got = gdg::sigma_parallax_from_g(r.g);
    REQUIRE(got.size() == want.size());
    for (size_t i = 0; i < got.size(); ++i) CHECK(same(got[i], want[i]));
    // The knot table itself, clamping, and NaN.
    CHECK(gdg::sigma_parallax_from_g(13.0) == 0.02);
    CHECK(gdg::sigma_parallax_from_g(17.5) == Catch::Approx(0.065).epsilon(1e-15));
    CHECK(gdg::sigma_parallax_from_g(18.0) == 0.08);
    CHECK(gdg::sigma_parallax_from_g(2.0) == 0.02);
    CHECK(gdg::sigma_parallax_from_g(30.0) == 0.65);
    CHECK(std::isnan(gdg::sigma_parallax_from_g(NAN)));
}

TEST_CASE("parallax_table_matches_golden", "[distances][golden]") {
    const Raw r = raw();
    Captured cap;
    check_catalog(gdg::parallax_to_distance_table(r.ra, r.dec, r.plx, r.g, r.bp, 5.0, cap.log()), "catalog");
    check_catalog(gdg::parallax_to_distance_table(r.ra, r.dec, r.plx, r.g, r.bp, 5.0, cap.log()), "mono_catalog");
}

TEST_CASE("snr_cut_logs_drop_count", "[distances][golden]") {
    const Raw r = raw();
    Captured cap;
    gdg::parallax_to_distance_table(r.ra, r.dec, r.plx, r.g, r.bp, 5.0, cap.log());
    REQUIRE(cap.warning.size() == 1);
    CHECK(cap.warning[0] ==
          "local Gaia: dropped 6/47 sources failing the parallax S/N >= 5.0 cut (non-positive or low-S/N "
          "parallax \xE2\x80\x94 distance unreliable). Kept 41.");
    // Nothing dropped: no warning. S/N exactly at the cut is kept (>=).
    Captured quiet;
    const auto t = gdg::parallax_to_distance_table({1.0, 2.0}, {3.0, 4.0}, {0.1, 1.0}, {13.0, 13.0}, {}, 5.0,
                                                   quiet.log());  // S/N 5 and 50
    CHECK(quiet.warning.empty());
    REQUIRE(t.size() == 2);
    CHECK(std::isnan(t[0].bp_rp));
    CHECK(t[1].source_id == 1);
    CHECK(t[0].r_med == 1000.0 / 0.1);
    CHECK(t[0].r_lo == 1000.0 / (0.1 + 0.02));
    CHECK(t[0].r_hi == 1000.0 / (0.1 - 0.02));
}

TEST_CASE("non_positive_parallax_dropped_even_without_snr_cut", "[distances]") {
    // keep = isfinite(plx) & (plx > 0) & ...: a zero parallax never becomes an infinite distance.
    Captured cap;
    const auto t = gdg::parallax_to_distance_table({1.0, 2.0, 3.0}, {0.0, 0.0, 0.0}, {0.0, -1.0, 2.0},
                                                   {12.0, 12.0, 12.0}, {}, 0.0, cap.log());
    REQUIRE(t.size() == 1);
    CHECK(t[0].ra == 3.0);
    CHECK(cap.warning.size() == 1);
}

TEST_CASE("parallax_lower_bound_is_clipped", "[distances]") {
    // plx - sigma clipped to 1e-6 mas: S/N 5 at plx = 5 sigma never reaches it, so use snr 1.
    const auto t = gdg::parallax_to_distance_table({1.0}, {2.0}, {0.02}, {13.0}, {0.7}, 1.0, gdg::Log{});
    REQUIRE(t.size() == 1);
    CHECK(t[0].r_hi == 1000.0 / 1e-6);
    CHECK(t[0].bp_rp == 0.7);
}

TEST_CASE("zero_usable_throws", "[distances]") {
    Captured cap;
    try {
        gdg::local_gaia_distances({1.0, 2.0}, {3.0, 4.0}, {-0.5, NAN}, {12.0, 12.0}, {0.3, 0.4}, 18.0, 5.0, cap.log());
        FAIL("no exception");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) ==
              "local Gaia produced zero usable sources after the parallax/magnitude cuts for this field");
    }
    CHECK(cap.info.empty());
}

TEST_CASE("local_gaia_magnitude_cut_and_bp_rp_passthrough", "[distances][golden]") {
    // The offline path keeps Gaia BP-RP (the shipped product dropped it: the TSV had 4 columns).
    const Raw r = raw();
    Captured cap;
    const gdg::Catalog c = gdg::local_gaia_distances(r.ra, r.dec, r.plx, r.g, r.bp, 18.0, 5.0, cap.log());
    check_catalog(c, "catalog");  // every golden G is < 18 (uniform 9..17.5)
    size_t finite_bp = 0;
    for (const auto& row : c) finite_bp += std::isfinite(row.bp_rp) ? 1 : 0;
    CHECK(finite_bp >= 30);
    CHECK(cap.info == std::vector<std::string>{"local Gaia: 41 usable sources (parallax distances)"});

    // Strict G < limit; G == limit and NaN G are excluded and counted.
    Captured cut;
    const gdg::Catalog c2 = gdg::local_gaia_distances({1, 2, 3, 4}, {0, 0, 0, 0}, {1.0, 1.0, 1.0, 1.0},
                                                      {17.9, 18.0, NAN, 12.0}, {0.1, 0.2, 0.3, 0.4}, 18.0, 5.0,
                                                      cut.log());
    REQUIRE(cut.warning.size() == 1);
    CHECK(cut.warning[0] == "local Gaia: magnitude cut G<18.0 excludes 2 source(s).");
    REQUIRE(c2.size() == 2);
    CHECK(c2[0].ra == 1.0);
    CHECK(c2[0].bp_rp == 0.1);
    CHECK(c2[1].ra == 4.0);
    CHECK(c2[1].bp_rp == 0.4);
    CHECK(c2[1].source_id == 1);

    // No limit: nothing cut, no warning.
    Captured none;
    CHECK(gdg::local_gaia_distances({1, 2}, {0, 0}, {10.0, 10.0}, {19.5, 12.0}, {}, std::nullopt, 5.0, none.log()).size() ==
          2);
    CHECK(none.warning.empty());
}

TEST_CASE("settings_discovery", "[distances]") {
    const std::string both =
        "<i k=\"Gaia\"><v k=\"DR3DatabaseFilePath00\" t=\"s\">/opt/gia3/gdr3-1.0.0-01.xpsd</v>"
        "<v k=\"DR3DatabaseFilePath01\" t=\"s\">/opt/gia3/gdr3-1.0.0-02.xpsd</v>"
        "<v k=\"DR3SPDatabaseFilePath00\" t=\"s\">/opt/gia3/gdr3sp-1.0.0-01.xpsd</v></i>";
    CHECK(gdg::gaia_db_paths_from_settings(both) ==
          std::vector<std::string>{"/opt/gia3/gdr3-1.0.0-01.xpsd", "/opt/gia3/gdr3-1.0.0-02.xpsd"});
    const std::string sp_only =
        "<v k=\"EDR3DatabaseFilePath00\" t=\"s\">/x/edr3-01.xpsd</v>"
        "<v k=\"DR3SPDatabaseFilePath00\" t=\"s\">/a/b &amp; c/gdr3sp-1.0.0-01.xpsd</v>"
        "<v k=\"DR3SPDatabaseFilePath01\" t=\"s\">/a/gdr3sp-1.0.0-02.xpsd</v>"
        "<v k=\"DR3SPDatabaseFilePath02\" t=\"s\">/a/not-a-database.txt</v>";
    // EDR3 keys are not DR3 keys (the PJSR regex was unanchored and would have taken them).
    CHECK(gdg::gaia_db_paths_from_settings(sp_only) ==
          std::vector<std::string>{"/a/b & c/gdr3sp-1.0.0-01.xpsd", "/a/gdr3sp-1.0.0-02.xpsd"});
    CHECK(gdg::gaia_db_paths_from_settings("<i k=\"Gaia\"></i>").empty());
}
