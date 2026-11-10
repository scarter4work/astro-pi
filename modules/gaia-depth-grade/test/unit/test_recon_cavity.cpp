// Task 18 (recon plan task 8): reconstruct_cavity against the synthetic_blister goldens (SHO
// default config in full; duoband / OSC / stretched / non-default config z + meta), plus the edge
// cases of gaia-depth-grade/tests/recon/test_cavity.py.
#include <catch_amalgamated.hpp>

#include "ReconGolden.h"
#include "core/Numerics.h"
#include "core/recon/Cavity.h"
#include "core/recon/Channels.h"
#include "core/recon/Ionization.h"
#include "core/recon/Ionizer.h"

#include <cmath>
#include <stdexcept>

using namespace gdg::recon;
using gdg::Image;
using gdg::test::check_golden_image;
using gdg::test::crop;
using gdg::test::golden_image;

namespace {

constexpr int CY0 = 40, CY1 = 110, CX0 = 10, CX1 = 100;  // stage.json recon_crop

Ionizer blister_ionizer() { return Ionizer{50.0, 86.0, 950.0, "gaia-blue"}; }  // stage.json recon.ionizer

CavityModel blister_model(std::optional<double> confidence = std::nullopt, bool with_sii = true) {
    ChannelSet ch = gdg::test::blister_channels();
    if (!with_sii) ch = ChannelSet(ch.ha, ch.oiii, std::nullopt, ch.ratio_confidence, ch.is_linear);
    IonizationMap ion = ionization_ratio(ch);
    if (confidence) ion.confidence = *confidence;
    return reconstruct_cavity(ch, ion, blister_ionizer(), golden_image("recon_lum"), golden_image("recon_dust_in"), 0.015,
                              ReconConfig{});
}

struct ExpectedMeta {
    double w_geometry, w_ionization_max, w_ionization_effective, w_emission, emission_sign, w_dust,
        ionization_confidence, ratio_confidence;
    bool is_linear, sii_present;
    std::string capture_mode, ionizer_source;
    double footprint_frac;
    long long footprint_px;
};

void check_meta(const CavityMeta& m, const ExpectedMeta& e) {
    CHECK(m.w_geometry == e.w_geometry);
    CHECK(m.w_ionization_max == e.w_ionization_max);
    CHECK(m.w_ionization_effective == e.w_ionization_effective);
    CHECK(m.w_emission == e.w_emission);
    CHECK(m.emission_sign == e.emission_sign);
    CHECK(m.w_dust == e.w_dust);
    CHECK(m.ionization_confidence == e.ionization_confidence);
    CHECK(m.ratio_confidence == e.ratio_confidence);
    CHECK(m.is_linear == e.is_linear);
    CHECK(m.sii_present == e.sii_present);
    CHECK(m.capture_mode == e.capture_mode);
    CHECK(m.ionizer_source == e.ionizer_source);
    CHECK(m.footprint_frac == e.footprint_frac);
    CHECK(m.footprint_px == e.footprint_px);
}

Image add_f32(const Image& a, const Image& b) {  // (a + b).astype(np.float32) in float64
    Image out = a;
    for (size_t i = 0; i < out.data.size(); ++i) out.data[i] = f32(a.data[i] + b.data[i]);
    return out;
}

Image sqrt_of(const Image& a) {
    Image out = a;
    for (double& v : out.data) v = std::sqrt(v);
    return out;
}

Image stack3(const Image& r, const Image& g, const Image& b) {
    Image out(r.width, r.height, 3);
    for (size_t i = 0; i < r.pixels(); ++i) {
        out.data[3 * i] = r.data[i];
        out.data[3 * i + 1] = g.data[i];
        out.data[3 * i + 2] = b.data[i];
    }
    return out;
}

/// The fixture's truth masks (tests/recon/conftest.py synthetic_blister).
struct Truth {
    Mask footprint, oiii_region, ha_region, lane;
};

Truth blister_truth() {
    const int n = 140;
    Truth t{Mask(n, n), Mask(n, n), Mask(n, n), Mask(n, n)};
    const double ax = 50, ay = 86, R = 50, ox = 94, oy = 54, sigma = 13;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const size_t i = static_cast<size_t>(y) * n + x;
            const double ra = std::hypot(x - ax, y - ay), d = std::hypot(x - ox, y - oy);
            const bool foot = ra <= R;
            const double ha_n = std::exp(-(ra * ra) / (2 * sigma * sigma)), o_n = std::exp(-(d * d) / (2 * sigma * sigma));
            t.footprint.data[i] = foot;
            t.oiii_region.data[i] = foot && o_n > 0.4 && ha_n < 0.2;
            t.ha_region.data[i] = foot && ha_n > 0.4 && o_n < 0.2;
            t.lane.data[i] = foot && x >= 70 && x <= 76 && y >= 60 && y <= 96;
        }
    return t;
}

double mean_where(const Image& z, const Mask& m, bool want = true) {
    double s = 0;
    size_t n = 0;
    for (size_t i = 0; i < z.data.size(); ++i)
        if (static_cast<bool>(m.data[i]) == want) {
            s += z.data[i];
            ++n;
        }
    return s / static_cast<double>(n);
}

Mask rim(const Truth& t) {
    Mask r = t.footprint;
    for (size_t i = 0; i < r.data.size(); ++i) r.data[i] = t.footprint.data[i] && !t.ha_region.data[i] && !t.oiii_region.data[i];
    return r;
}

Mask and_not(const Mask& a, const Mask& b) {
    Mask r = a;
    for (size_t i = 0; i < r.data.size(); ++i) r.data[i] = a.data[i] && !b.data[i];
    return r;
}

double apex_distance_of_max(const Image& z, const Mask& foot) {
    size_t best = 0;
    double bz = -INFINITY;
    for (size_t i = 0; i < z.data.size(); ++i)
        if (foot.data[i] && z.data[i] > bz) {
            bz = z.data[i];
            best = i;
        }
    const int ix = static_cast<int>(best % z.width), iy = static_cast<int>(best / z.width);
    return std::hypot(ix - 50.0, iy - 86.0);
}

}  // namespace

TEST_CASE("reconstruct_cavity_sho_matches_golden", "[recon][cavity][golden]") {
    const CavityModel m = blister_model();
    check_golden_image(m.z, "recon_cavity_z", 0.0, 0);
    check_golden_image(m.emissivity, "recon_cavity_emissivity", 0.0, 0);
    // stage.json recon_cavity_dust: model.dust == clip(recon_dust_in, 0, 1)
    Image want_dust = golden_image("recon_dust_in");
    for (double& v : want_dust.data) v = f32(clip(v, 0.0, 1.0));
    CHECK(m.dust.data == want_dust.data);
    CHECK(m.apex_xy == std::pair<double, double>{50.0, 86.0});
    CHECK(m.pc_per_px == 0.015);
    check_meta(m.meta, {1.0, 0.6, 0.6, 0.15, 1.0, 0.8, 1.0, 1.0, true, true, "sho", "gaia-blue", 0.05, 7845});
}

TEST_CASE("reconstruct_cavity_capture_modes_match_golden", "[recon][cavity][golden]") {
    const ChannelSet full = gdg::test::blister_channels();
    const Image ha = crop(full.ha, CY0, CY1, CX0, CX1), oiii = crop(full.oiii, CY0, CY1, CX0, CX1),
                sii = crop(*full.sii, CY0, CY1, CX0, CX1);
    const Image lum = crop(golden_image("recon_lum"), CY0, CY1, CX0, CX1);
    const Image dust = crop(golden_image("recon_dust_in"), CY0, CY1, CX0, CX1);
    const Ionizer ion = ionizer_from_xy(40.0, 46.0, 950.0);

    const ChannelSet duo = from_duoband(add_f32(ha, oiii), add_f32(sii, oiii));
    const CavityModel md = reconstruct_cavity(duo, ionization_ratio(duo), ion, lum, dust, 0.015, ReconConfig{});
    check_golden_image(md.z, "recon_crop_cavity_z_duoband", 0.0, 0);
    CHECK(md.apex_xy == std::pair<double, double>{40.0, 46.0});
    check_meta(md.meta, {1.0, 0.6, 0.36, 0.15, 1.0, 0.8, 0.6, 0.6, true, true, "duoband", "override", 0.05, 3819});

    const ChannelSet osc = from_osc_rgb(stack3(ha, oiii, oiii));
    const CavityModel mo = reconstruct_cavity(osc, ionization_ratio(osc), ion, lum, dust, 0.015, ReconConfig{});
    check_golden_image(mo.z, "recon_crop_cavity_z_osc", 0.0, 0);
    check_meta(mo.meta, {1.0, 0.6, 0.15, 0.15, 1.0, 0.8, 0.25, 0.25, true, false, "osc", "override", 0.05, 3819});

    const ChannelSet st = extract_channels({{"ha", sqrt_of(ha)}, {"oiii", sqrt_of(oiii)}, {"sii", sqrt_of(sii)}}, "sho");
    const IonizationMap st_ion = ionization_ratio(st);
    CHECK(st_ion.confidence == 0.0);  // stage.json recon_cavity_stretched ionization_confidence
    const CavityModel ms = reconstruct_cavity(st, st_ion, ion, lum, dust, 0.015, ReconConfig{});
    check_golden_image(ms.z, "recon_crop_cavity_z_stretched", 0.0, 0);
    check_meta(ms.meta, {1.0, 0.6, 0.0, 0.15, 1.0, 0.8, 0.0, 0.0, false, true, "nonlinear", "override", 0.05, 3819});

    // sii None, no distance scale, field-centre apex, every weight non-default (stage.json recon_cavity_cfg2)
    ReconConfig cfg2;
    cfg2.w_geometry = 0.7;
    cfg2.w_ionization = 1.2;
    cfg2.w_emission = 0.3;
    cfg2.emission_sign = -1.0;
    cfg2.w_dust = 0.5;
    cfg2.footprint_frac = 0.1;
    cfg2.geometry_power = 1.5;
    cfg2.ionization_gamma = 0.8;
    const Ionizer centre = pick_ionizer(nullptr, lum.height, lum.width);
    const CavityModel m2 = reconstruct_cavity(osc, ionization_ratio(osc), centre, lum, dust, std::nullopt, cfg2);
    // geometry power 1.5 and gamma 0.8 go through np.power: plan bound 1e-6, measured bit-identical
    check_golden_image(m2.z, "recon_crop_cavity_z_cfg2", 1e-6, 0);
    CHECK(m2.apex_xy == std::pair<double, double>{45.0, 35.0});
    CHECK_FALSE(m2.pc_per_px);
    check_meta(m2.meta, {0.7, 1.2, 0.3, 0.3, -1.0, 0.5, 0.25, 0.25, true, false, "osc", "field-center", 0.1, 2995});
}

TEST_CASE("cavity_meta_json_keys_verbatim", "[recon][cavity]") {
    // json.dumps(model.meta) for the SHO default case, keys in the Python dict's order.
    CHECK(cavity_meta_json(blister_model().meta) ==
          "{\"w_geometry\": 1.0, \"w_ionization_max\": 0.6, \"w_ionization_effective\": 0.6, \"w_emission\": 0.15, "
          "\"emission_sign\": 1.0, \"w_dust\": 0.8, \"ionization_confidence\": 1.0, \"ratio_confidence\": 1.0, "
          "\"is_linear\": true, \"sii_present\": true, \"capture_mode\": \"sho\", \"ionizer_source\": \"gaia-blue\", "
          "\"footprint_frac\": 0.05, \"footprint_px\": 7845}");
    // Python float repr: exponent form below 1e-4 and from 1e16, fixed otherwise
    CavityMeta m;
    m.w_geometry = 1e-05;
    m.w_ionization_max = 0.0001;
    m.w_ionization_effective = 1e16;
    m.w_emission = 123456789012345.0;
    m.emission_sign = -2.5e-07;
    m.w_dust = 1.5e+300;
    m.capture_mode = "a\"b";
    const std::string j = cavity_meta_json(m);
    CHECK(j.find("\"w_geometry\": 1e-05,") != std::string::npos);
    CHECK(j.find("\"w_ionization_max\": 0.0001,") != std::string::npos);
    CHECK(j.find("\"w_ionization_effective\": 1e+16,") != std::string::npos);
    CHECK(j.find("\"w_emission\": 123456789012345.0,") != std::string::npos);
    CHECK(j.find("\"emission_sign\": -2.5e-07,") != std::string::npos);
    CHECK(j.find("\"w_dust\": 1.5e+300,") != std::string::npos);
    CHECK(j.find("\"capture_mode\": \"a\\\"b\"") != std::string::npos);
    CHECK(j.find("\"is_linear\": false") != std::string::npos);
}

TEST_CASE("reconstructs_concave_cavity_with_apex", "[recon][cavity]") {
    const Truth t = blister_truth();
    const CavityModel m = blister_model();
    CHECK(m.emissivity.channels == 3);
    for (double v : m.z.data) {
        REQUIRE(std::isfinite(v));
        REQUIRE(v >= 0.0);
        REQUIRE(v <= 1.0);
    }
    CHECK(apex_distance_of_max(m.z, t.footprint) < 12.0);
    CHECK(mean_where(m.z, t.ha_region) > mean_where(m.z, rim(t)));
}

TEST_CASE("oiii_interior_placed_forward_of_front", "[recon][cavity]") {
    const Truth t = blister_truth();
    const CavityModel m = blister_model();
    CHECK(mean_where(m.z, t.oiii_region) < mean_where(m.z, t.ha_region));
}

TEST_CASE("dust_lane_is_near_side", "[recon][cavity]") {
    const Truth t = blister_truth();
    const CavityModel m = blister_model();
    CHECK(mean_where(m.z, t.lane) < mean_where(m.z, and_not(t.footprint, t.lane)));
    const Image d = golden_image("recon_dust_in");
    for (size_t i = 0; i < d.data.size(); ++i) CHECK(m.dust.data[i] == Catch::Approx(d.data[i]).margin(1e-8));
}

TEST_CASE("low_ratio_confidence_still_concave", "[recon][cavity]") {
    const Truth t = blister_truth();
    const CavityModel m = blister_model(0.05);
    CHECK(apex_distance_of_max(m.z, t.footprint) < 12.0);
    CHECK(mean_where(m.z, t.ha_region) - mean_where(m.z, rim(t)) > 0.05);
    CHECK(m.meta.w_ionization_effective == 0.6 * 0.05);
}

TEST_CASE("sii_none_supported", "[recon][cavity]") {
    const Truth t = blister_truth();
    const CavityModel m = blister_model(std::nullopt, false);
    for (double v : m.z.data) REQUIRE(std::isfinite(v));
    CHECK(mean_where(m.z, t.ha_region) > mean_where(m.z, rim(t)));
    CHECK_FALSE(m.meta.sii_present);
}

TEST_CASE("reconstruct_cavity_edges", "[recon][cavity]") {
    const ChannelSet ch = gdg::test::blister_channels();
    const IonizationMap ion = ionization_ratio(ch);
    const Image lum = golden_image("recon_lum"), dust = golden_image("recon_dust_in");
    // misregistered planes are a hard error
    CHECK_THROWS_AS(reconstruct_cavity(ch, ion, blister_ionizer(), Image(10, 10), dust, std::nullopt, ReconConfig{}),
                    std::invalid_argument);
    CHECK_THROWS_AS(reconstruct_cavity(ch, ion, blister_ionizer(), lum, Image(140, 139), std::nullopt, ReconConfig{}),
                    std::invalid_argument);
    CHECK_THROWS_AS(reconstruct_cavity(ch, IonizationMap{Image(5, 5), 1.0}, blister_ionizer(), lum, dust, std::nullopt,
                                       ReconConfig{}),
                    std::invalid_argument);
    // degenerate weights fall back to the geometric bowl; confidence is clipped to [0, 1]
    ReconConfig zero;
    zero.w_geometry = -1.0;
    zero.w_ionization = 0.0;
    zero.w_emission = 0.0;
    zero.w_dust = 0.0;
    IonizationMap hot = ion;
    hot.confidence = 7.0;
    const CavityModel g = reconstruct_cavity(ch, hot, blister_ionizer(), lum, dust, std::nullopt, zero);
    CHECK(g.meta.w_geometry == 0.0);
    CHECK(g.meta.ionization_confidence == 1.0);
    const Mask foot = footprint_from_lum(lum, zero.footprint_frac);
    CHECK(g.z.data == geometry_prior(140, 140, {50.0, 86.0}, &foot).data);
    // model.dust is clip(nan_to_num(dust), 0, 1)
    Image wild = dust;
    wild.data[0] = 3.0;
    wild.data[1] = -2.0;
    wild.data[2] = std::nan("");
    const CavityModel w = reconstruct_cavity(ch, ion, blister_ionizer(), lum, wild, std::nullopt, ReconConfig{});
    CHECK(w.dust.data[0] == 1.0);
    CHECK(w.dust.data[1] == 0.0);
    CHECK(w.dust.data[2] == 0.0);
    // w_dust is clipped to [0, 1] in the blend but reported raw
    ReconConfig heavy;
    heavy.w_dust = 5.0;
    const CavityModel h = reconstruct_cavity(ch, ion, blister_ionizer(), lum, dust, std::nullopt, heavy);
    CHECK(h.meta.w_dust == 5.0);
    ReconConfig one;
    one.w_dust = 1.0;
    CHECK(h.z.data == reconstruct_cavity(ch, ion, blister_ionizer(), lum, dust, std::nullopt, one).z.data);
}

TEST_CASE("capture_mode_rules", "[recon][cavity]") {
    const Image p(4, 4);
    CHECK(capture_mode(ChannelSet(p, p, p, 1.0, true)) == "sho");
    CHECK(capture_mode(ChannelSet(p, p, std::nullopt, 1.0, true)) == "osc");  // SHO confidence without sii
    CHECK(capture_mode(ChannelSet(p, p, p, 0.6, true)) == "duoband");
    CHECK(capture_mode(ChannelSet(p, p, p, 0.25, true)) == "osc");
    CHECK(capture_mode(ChannelSet(p, p, std::nullopt, 0.0, false)) == "osc");  // Python's order: osc before nonlinear
    CHECK(capture_mode(ChannelSet(p, p, p, 0.0, false)) == "nonlinear");
    CHECK(capture_mode(ChannelSet(p, p, p, 0.5, true)) == "unknown");
    CHECK(capture_mode(ChannelSet(p, p, p, 1.0 + 5e-7, true)) == "sho");  // within 1e-6
    CHECK(capture_mode(ChannelSet(p, p, p, 1.0 + 2e-6, true)) == "unknown");
}

TEST_CASE("linear_emissivity_mapping", "[recon][cavity]") {
    Image ha(2, 1), oiii(2, 1), sii(2, 1);
    ha.data = {0.4, std::nan("")};
    oiii.data = {0.2, -1.0};
    sii.data = {0.1, 0.0};
    const Image e = linear_emissivity(ChannelSet(ha, oiii, sii, 1.0, true));
    // peak = 99.9th percentile of [0.4, 0, 0.2, 0, 0.1, 0] (NaN and negatives cleaned to 0)
    const double peak = gdg::percentile({0.4, 0.0, 0.2, 0.0, 0.1, 0.0}, 99.9);
    CHECK(e.channels == 3);
    CHECK(e.data[0] == f32((0.4 + 0.1) / peak));
    CHECK(e.data[1] == f32(0.5 * 0.2 / peak));
    CHECK(e.data[2] == f32(0.2 / peak));
    CHECK(e.data[3] == 0.0);
    CHECK(e.data[4] == 0.0);
    CHECK(e.data[5] == 0.0);
    // all zero: peak falls back to 1
    for (double v : linear_emissivity(ChannelSet(Image(3, 3), Image(3, 3), std::nullopt, 0.25, true)).data) CHECK(v == 0.0);
}
