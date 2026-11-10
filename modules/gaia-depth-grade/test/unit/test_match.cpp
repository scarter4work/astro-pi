// Task 11: field footprint (wcs.py) and cross-match (match.py) against the Task 1 goldens.
//
// The golden constants below are copied from golden/synthetic/stage.json (wcs, fixture_footprint
// and match_stats / mono_match_stats).
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "TanSky.h"
#include "core/Footprint.h"
#include "core/Match.h"

#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using gdg::test::golden;
using gdg::test::TanSky;

namespace {

// stage.json "wcs": the synthetic field's header (192 x 144).
TanSky field_sky() {
    return TanSky::from_keywords({{"CRPIX1", 96.0},
                                  {"CRPIX2", 72.0},
                                  {"CRVAL1", 10.0},
                                  {"CRVAL2", 20.0},
                                  {"CDELT1", -0.00027777777777778},
                                  {"CDELT2", 0.00027777777777778},
                                  {"LONPOLE", 180.0}});
}

// A transform that only knows the golden catalogue rows: sky_to_pixel returns astropy's own
// world_to_pixel_values result for an exact (ra, dec) of the golden catalogue, so the matcher is
// tested in isolation from the WCS.
class LookupSky : public gdg::SkyTransform {
public:
    LookupSky(const std::vector<double>& ra, const std::vector<double>& dec, const std::vector<double>& xy) {
        for (size_t i = 0; i < ra.size(); ++i) map_[{ra[i], dec[i]}] = {xy[2 * i], xy[2 * i + 1]};
    }
    bool pixel_to_sky(double, double, double&, double&) const override { return false; }
    bool sky_to_pixel(double ra, double dec, double& x, double& y) const override {
        auto it = map_.find({ra, dec});
        if (it == map_.end()) throw std::logic_error("LookupSky: unknown (ra, dec)");
        x = it->second.first;
        y = it->second.second;
        return true;
    }

private:
    std::map<std::pair<double, double>, std::pair<double, double>> map_;
};

// A plain shift: sky (ra, dec) is just pixel (x, y); "unprojectable" when ra is NaN.
class IdentitySky : public gdg::SkyTransform {
public:
    bool pixel_to_sky(double x, double y, double& ra, double& dec) const override {
        ra = x;
        dec = y;
        return true;
    }
    bool sky_to_pixel(double ra, double dec, double& x, double& y) const override {
        if (std::isnan(ra)) return false;
        x = ra;
        y = dec;
        return true;
    }
};

std::vector<gdg::StarRow> detections(const std::string& prefix) {
    const auto x = golden(prefix + "_x").data, y = golden(prefix + "_y").data, f = golden(prefix + "_flux").data;
    std::vector<gdg::StarRow> t;
    for (size_t i = 0; i < x.size(); ++i) t.push_back(gdg::StarRow{x[i], y[i], f[i]});
    return t;
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

bool same(double a, double b) { return (std::isnan(a) && std::isnan(b)) || a == b; }

void check_match_golden(const std::string& p, int n_det, int n_match, double rate, double median) {
    const auto det = detections(p + "detect_table");
    const gdg::Catalog cat = catalog(p + "catalog");
    const LookupSky sky(golden(p + "catalog_ra").data, golden(p + "catalog_dec").data, golden(p + "catalog_xy").data);
    gdg::MatchStats st;
    const auto out = gdg::cross_match(det, cat, sky, 4.0, st);
    REQUIRE(out.size() == det.size());
    const auto mx = golden(p + "match_table_x").data, my = golden(p + "match_table_y").data,
               mf = golden(p + "match_table_flux").data, mm = golden(p + "match_table_matched").data,
               rm = golden(p + "match_table_r_med_geo").data, rl = golden(p + "match_table_r_lo_geo").data,
               rh = golden(p + "match_table_r_hi_geo").data, bp = golden(p + "match_table_bp_rp").data;
    size_t bad = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const auto& r = out[i];
        if (r.x != mx[i] || r.y != my[i] || r.flux != mf[i] || r.matched != (mm[i] != 0.0) || !same(r.r_med, rm[i]) ||
            !same(r.r_lo, rl[i]) || !same(r.r_hi, rh[i]) || !same(r.bp_rp, bp[i]))
            ++bad;
    }
    CHECK(bad == 0);
    CHECK(st.n_detected == n_det);
    CHECK(st.n_matched == n_match);
    CHECK(st.match_rate == rate);
    CHECK(st.median_offset_px == median);
}

}  // namespace

TEST_CASE("separation_is_astropy_vincenty", "[footprint]") {
    CHECK(gdg::angular_separation_deg(10.0, 20.0, 10.0, 20.0) == 0.0);
    CHECK(gdg::angular_separation_deg(0.0, 0.0, 90.0, 0.0) == Catch::Approx(90.0).margin(1e-12));
    CHECK(gdg::angular_separation_deg(0.0, -90.0, 123.0, 90.0) == Catch::Approx(180.0).margin(1e-12));
    // Antipode-stable and symmetric
    CHECK(gdg::angular_separation_deg(359.5, 10.0, 0.5, -10.0) ==
          Catch::Approx(gdg::angular_separation_deg(0.5, -10.0, 359.5, 10.0)).margin(1e-14));
}

TEST_CASE("footprint_matches_astropy", "[footprint][golden]") {
    // The field header (192 x 144) and the conftest simple_wcs_header (300 x 200).
    const auto want = golden("footprint").data;
    const gdg::Footprint fp = gdg::field_footprint(field_sky(), 144, 192);
    WARN("footprint |delta| ra " << std::fabs(fp.center_ra - want[0]) << ", dec " << std::fabs(fp.center_dec - want[1])
                                 << ", radius " << std::fabs(fp.radius_deg - want[2]) << " deg");
    CHECK(std::fabs(fp.center_ra - want[0]) <= 1e-10);
    CHECK(std::fabs(fp.center_dec - want[1]) <= 1e-10);
    CHECK(std::fabs(fp.radius_deg - want[2]) <= 1e-10);
    CHECK(golden("mono_footprint").data == want);

    const auto fx = golden("fixture_footprint").data;
    const TanSky fixture = TanSky::from_keywords({{"CRPIX1", 150.0},
                                                  {"CRPIX2", 100.0},
                                                  {"CRVAL1", 10.0},
                                                  {"CRVAL2", 20.0},
                                                  {"CDELT1", -0.00027777777777778},
                                                  {"CDELT2", 0.00027777777777778},
                                                  {"LONPOLE", 180.0}});
    const gdg::Footprint f2 = gdg::field_footprint(fixture, 200, 300);
    CHECK(std::fabs(f2.center_ra - fx[0]) <= 1e-10);
    CHECK(std::fabs(f2.center_dec - fx[1]) <= 1e-10);
    CHECK(std::fabs(f2.radius_deg - fx[2]) <= 1e-10);
}

TEST_CASE("tansky_matches_astropy_world_to_pixel", "[footprint][golden]") {
    // TanSky is the test oracle for the pipeline and the in-PI probe: its sky_to_pixel must
    // reproduce astropy's catalog_xy for every golden catalogue row.
    const TanSky sky = field_sky();
    const auto ra = golden("catalog_ra").data, dec = golden("catalog_dec").data, xy = golden("catalog_xy").data;
    double worst = 0.0;
    for (size_t i = 0; i < ra.size(); ++i) {
        double x, y;
        REQUIRE(sky.sky_to_pixel(ra[i], dec[i], x, y));
        worst = std::max({worst, std::fabs(x - xy[2 * i]), std::fabs(y - xy[2 * i + 1])});
        double r2, d2;
        REQUIRE(sky.pixel_to_sky(x, y, r2, d2));
        CHECK(std::fabs(r2 - ra[i]) <= 1e-11);
        CHECK(std::fabs(d2 - dec[i]) <= 1e-11);
    }
    WARN("TanSky vs astropy world_to_pixel: max |delta| " << worst << " px");
    CHECK(worst <= 1e-9);
}

TEST_CASE("footprint_throws_when_unprojectable", "[footprint]") {
    class Never : public gdg::SkyTransform {
        bool pixel_to_sky(double, double, double&, double&) const override { return false; }
        bool sky_to_pixel(double, double, double&, double&) const override { return false; }
    };
    CHECK_THROWS_AS(gdg::field_footprint(Never{}, 10, 10), std::runtime_error);
}

TEST_CASE("match_matches_golden", "[match][golden]") {
    // stage.json match_stats and mono_match_stats (tolerance 4 px).
    check_match_golden("", 37, 26, 0.7027027027027027, 0.9221258377147912);
    check_match_golden("mono_", 37, 26, 0.7027027027027027, 0.9221258851340097);
}

TEST_CASE("match_empty_inputs", "[match]") {
    const IdentitySky sky;
    gdg::MatchStats st;
    st.n_matched = 99;
    auto out = gdg::cross_match({}, {gdg::CatalogRow{1.0, 1.0, 10.0, 9.0, 11.0, 1}}, sky, 4.0, st);
    CHECK(out.empty());
    CHECK(st.n_detected == 0);
    CHECK(st.n_matched == 0);
    CHECK(st.match_rate == 0.0);
    CHECK(std::isnan(st.median_offset_px));

    gdg::StarRow s{5.0, 5.0, 1.0};
    s.matched = true;  // stale annotations are reset
    s.r_med = 3.0;
    s.bp_rp = 0.5;
    out = gdg::cross_match({s, gdg::StarRow{7.0, 7.0, 1.0}}, {}, sky, 4.0, st);
    REQUIRE(out.size() == 2);
    CHECK(!out[0].matched);
    CHECK(std::isnan(out[0].r_med));
    CHECK(std::isnan(out[0].bp_rp));
    CHECK(st.n_detected == 2);
    CHECK(st.n_matched == 0);
    CHECK(st.match_rate == 0.0);
    CHECK(std::isnan(st.median_offset_px));
}

TEST_CASE("match_dict_semantics", "[match]") {
    const IdentitySky sky;
    const std::vector<gdg::StarRow> det{{10.0, 10.0, 1.0}, {20.0, 10.0, 1.0}, {40.0, 40.0, 1.0}, {13.0, 10.0, 1.0}};
    gdg::Catalog cat;
    auto row = [&](double x, double y, double r, double bp) {
        cat.push_back(gdg::CatalogRow{x, y, r, r - 1.0, r + 1.0, static_cast<long long>(cat.size()), bp});
    };
    row(11.0, 10.0, 100.0, 0.5);   // det0 at d = 1.0
    row(10.5, 10.0, 200.0, 0.6);   // det0 at d = 0.5: strictly closer, replaces row 0
    row(9.5, 10.0, 300.0, 0.7);    // det0 at d = 0.5: a tie, the earlier row (1) stays
    row(24.0, 10.0, 400.0, NAN);   // det1 at exactly the tolerance (4): accepted
    row(40.0, 44.0000001, 500.0, 0.9);  // det2 just beyond the tolerance: rejected
    row(NAN, 0.0, 600.0, 1.0);     // cannot be projected: never matches
    row(14.0, 10.0, 700.0, 1.1);   // nearest is det3 (d = 1), not det1 (d = 6) or det0 (d = 4)
    gdg::MatchStats st;
    const auto out = gdg::cross_match(det, cat, sky, 4.0, st);
    CHECK(out[0].matched);
    CHECK(out[0].r_med == 200.0);
    CHECK(out[0].r_lo == 199.0);
    CHECK(out[0].r_hi == 201.0);
    CHECK(out[0].bp_rp == 0.6);
    CHECK(out[1].matched);
    CHECK(out[1].r_med == 400.0);
    CHECK(std::isnan(out[1].bp_rp));
    CHECK(!out[2].matched);
    CHECK(std::isnan(out[2].r_med));
    CHECK(out[3].matched);
    CHECK(out[3].r_med == 700.0);
    CHECK(out[0].x == 10.0);
    CHECK(out[3].flux == 1.0);
    CHECK(st.n_detected == 4);
    CHECK(st.n_matched == 3);
    CHECK(st.match_rate == 0.75);
    CHECK(st.median_offset_px == 1.0);  // median of {0.5, 4.0, 1.0}
}

TEST_CASE("match_large_field_nearest_neighbour", "[match]") {
    // A dense pseudo-random field: the matcher must equal a brute-force nearest neighbour
    // (lowest index on a tie) for every catalogue row, whatever its spatial index does.
    const IdentitySky sky;
    std::vector<gdg::StarRow> det;
    gdg::Catalog cat;
    unsigned s = 12345u;
    auto rnd = [&]() {
        s = s * 1103515245u + 12345u;
        return static_cast<double>((s >> 8) & 0xFFFF) / 65536.0;
    };
    for (int i = 0; i < 3000; ++i) det.push_back(gdg::StarRow{rnd() * 800.0, rnd() * 600.0, 1.0});
    for (int i = 0; i < 4000; ++i)
        cat.push_back(gdg::CatalogRow{rnd() * 820.0 - 10.0, rnd() * 620.0 - 10.0, 1.0 + i, 0.5 + i, 1.5 + i, i});
    const double tol = 2.5;
    gdg::MatchStats st;
    const auto out = gdg::cross_match(det, cat, sky, tol, st);
    // brute force reference
    std::vector<int> best(det.size(), -1);
    std::vector<double> bestd(det.size(), 0.0);
    for (size_t c = 0; c < cat.size(); ++c) {
        double dmin = INFINITY;
        int imin = -1;
        for (size_t i = 0; i < det.size(); ++i) {
            const double dx = cat[c].ra - det[i].x, dy = cat[c].dec - det[i].y;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d < dmin) {
                dmin = d;
                imin = static_cast<int>(i);
            }
        }
        if (dmin > tol) continue;
        if (best[imin] < 0 || dmin < bestd[imin]) {
            best[imin] = static_cast<int>(c);
            bestd[imin] = dmin;
        }
    }
    int n = 0;
    size_t bad = 0;
    for (size_t i = 0; i < det.size(); ++i) {
        if (best[i] >= 0) ++n;
        const bool m = best[i] >= 0;
        if (out[i].matched != m || (m && out[i].r_med != cat[best[i]].r_med)) ++bad;
    }
    CHECK(bad == 0);
    CHECK(st.n_matched == n);
    CHECK(n > 300);  // 388 for this seed: the case exercises plenty of pairs
}
