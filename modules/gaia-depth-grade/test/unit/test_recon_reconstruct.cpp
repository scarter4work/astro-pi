// Task 22 (Phase G2): the reconstruct pipeline (channels -> CavityModel). No Python goldens:
// assertions are analytic truth on the C++ synthetic blister (test/util/Blister.h).
#include <catch_amalgamated.hpp>

#include "Blister.h"
#include "core/recon/Reconstruct.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace gdg::recon;
using gdg::Image;
using gdg::StarRow;
using gdg::test::Blister;
using gdg::test::make_blister;

namespace {

constexpr double PI = 3.14159265358979323846;

ReconInputs sho_inputs(const Blister& b) {
    ReconInputs in;
    in.mode = "sho";
    in.channels = {{"ha", b.ha}, {"oiii", b.oiii}, {"sii", b.sii}};
    in.ionizer_override = ionizer_from_xy(b.truth.apex_x, b.truth.apex_y, std::nullopt);
    return in;
}

ReconInputs osc_inputs(const Blister& b) {
    ReconInputs in;
    in.mode = "osc";
    in.channels = {{"rgb", b.rgb}};
    in.ionizer_override = ionizer_from_xy(b.truth.apex_x, b.truth.apex_y, std::nullopt);
    return in;
}

/// Mean z over pixels with lo <= r/R <= hi about the blister apex.
double mean_z(const Image& z, const Blister& b, double lo, double hi) {
    double sum = 0.0;
    long n = 0;
    for (int y = 0; y < z.height; ++y)
        for (int x = 0; x < z.width; ++x) {
            const double rho = std::hypot(x - b.truth.apex_x, y - b.truth.apex_y) / b.truth.radius_px;
            if (rho >= lo && rho <= hi) {
                sum += z.at(x, y);
                ++n;
            }
        }
    REQUIRE(n > 0);
    return sum / static_cast<double>(n);
}

}  // namespace

TEST_CASE("reconstruct_sho_end_to_end_concave", "[recon][reconstruct]") {
    const Blister b = make_blister();
    const ReconResult r = reconstruct(sho_inputs(b), ReconConfig{}, gdg::Log{});
    CHECK(mean_z(r.model.z, b, 0.0, 0.1) > mean_z(r.model.z, b, 0.8, 1.0));
    CHECK(r.model.meta.capture_mode == "sho");
    CHECK(r.degraded.empty());
    CHECK(r.model.meta.is_linear);
    CHECK(r.model.apex_xy.first == b.truth.apex_x);
    CHECK(r.model.apex_xy.second == b.truth.apex_y);
    CHECK(r.lum.width == b.ha.width);
    CHECK(r.lum.height == b.ha.height);
}

TEST_CASE("reconstruct_duoband_rgb_uses_channel_mean", "[recon][reconstruct]") {
    const Blister b = make_blister();
    Image d1 = b.rgb, d2 = b.rgb;
    for (size_t i = 0; i < d2.data.size(); ++i) d2.data[i] = f32(0.7 * d2.data[(i + 3) % d2.data.size()] + 0.001);

    ReconInputs rgb_in;
    rgb_in.mode = "duoband";
    rgb_in.channels = {{"d1", d1}, {"d2", d2}};
    rgb_in.ionizer_override = ionizer_from_xy(b.truth.apex_x, b.truth.apex_y, std::nullopt);
    ReconInputs mono_in = rgb_in;
    mono_in.channels = {{"d1", channel_mean(d1)}, {"d2", channel_mean(d2)}};

    const ReconResult a = reconstruct(rgb_in, ReconConfig{}, gdg::Log{});
    const ReconResult m = reconstruct(mono_in, ReconConfig{}, gdg::Log{});
    CHECK(a.model.z.data == m.model.z.data);
    CHECK(a.model.emissivity.data == m.model.emissivity.data);
    CHECK(a.model.dust.data == m.model.dust.data);
    CHECK(a.ionization.oiii_fraction.data == m.ionization.oiii_fraction.data);
    CHECK(a.lum.data == m.lum.data);
    CHECK(a.model.meta.capture_mode == "duoband");

    // channel_mean itself: mono passes through, colour is the f32 mean of the channels.
    CHECK(channel_mean(b.ha).data == b.ha.data);
    const Image cm = channel_mean(d1);
    CHECK(cm.channels == 1);
    for (size_t i = 0; i < cm.data.size(); ++i)
        CHECK(cm.data[i] == f32((d1.data[3 * i] + d1.data[3 * i + 1] + d1.data[3 * i + 2]) / 3.0));
}

TEST_CASE("reconstruct_osc_low_confidence_still_concave", "[recon][reconstruct]") {
    const Blister b = make_blister();
    const ReconResult r = reconstruct(osc_inputs(b), ReconConfig{}, gdg::Log{});
    CHECK(r.model.meta.capture_mode == "osc");
    CHECK(r.model.meta.ratio_confidence == 0.25);
    CHECK(r.degraded.empty());
    CHECK(mean_z(r.model.z, b, 0.0, 0.1) > mean_z(r.model.z, b, 0.8, 1.0));
}

TEST_CASE("reconstruct_stretched_input_degrades", "[recon][reconstruct]") {
    const Blister b = make_blister();
    ReconInputs in = osc_inputs(b);
    Image& rgb = in.channels.at("rgb");
    const double k = 1.0e4, norm = std::asinh(k);
    for (double& v : rgb.data) v = f32(std::asinh(k * v) / norm);

    std::vector<std::string> warnings;
    gdg::Log log;
    log.warning = [&](const std::string& m) { warnings.push_back(m); };
    const ReconResult r = reconstruct(in, ReconConfig{}, log);
    CHECK(r.degraded == "stretched-input");
    CHECK(r.model.meta.ratio_confidence == 0.0);
    CHECK_FALSE(r.model.meta.is_linear);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0] ==
          "recon: input looks stretched (non-linear); ratio term off, geometry prior only. The result is not "
          "physically constrained.");

    // A linear input stays silent.
    warnings.clear();
    reconstruct(osc_inputs(b), ReconConfig{}, log);
    CHECK(warnings.empty());
}

TEST_CASE("reconstruct_mismatched_shapes_throw", "[recon][reconstruct]") {
    const Blister b = make_blister();
    ReconInputs in = sho_inputs(b);
    in.channels.at("oiii") = Image(b.ha.width - 1, b.ha.height);
    REQUIRE_THROWS_AS(reconstruct(in, ReconConfig{}, gdg::Log{}), std::invalid_argument);
    REQUIRE_THROWS_WITH(reconstruct(in, ReconConfig{}, gdg::Log{}), Catch::Matchers::ContainsSubstring("shape mismatch"));
}

TEST_CASE("reconstruct_hb_shape_mismatch_throws", "[recon][reconstruct]") {
    const Blister b = make_blister();
    ReconInputs in = sho_inputs(b);
    in.hb = Image(b.ha.width, b.ha.height + 1);
    REQUIRE_THROWS_AS(reconstruct(in, ReconConfig{}, gdg::Log{}), std::invalid_argument);
    REQUIRE_THROWS_WITH(reconstruct(in, ReconConfig{}, gdg::Log{}), Catch::Matchers::ContainsSubstring("shape mismatch"));

    // hb = ha / 5 on the left half: Ha/Hb = 5 exceeds Case B (2.86) there.
    Image hb = b.ha;
    for (int y = 0; y < hb.height; ++y)
        for (int x = 0; x < hb.width; ++x)
            if (x < b.truth.apex_x) hb.at(x, y) = b.ha.at(x, y) / 5.0;
    in.hb = hb;
    const ReconResult with_hb = reconstruct(in, ReconConfig{}, gdg::Log{});
    const ReconResult without = reconstruct(sho_inputs(b), ReconConfig{}, gdg::Log{});
    size_t greater = 0;
    for (size_t i = 0; i < without.model.dust.data.size(); ++i) {
        REQUIRE(with_hb.model.dust.data[i] >= without.model.dust.data[i]);
        greater += with_hb.model.dust.data[i] > without.model.dust.data[i] ? 1 : 0;
    }
    CHECK(greater > 0);
}

TEST_CASE("reconstruct_scale_guards", "[recon][reconstruct]") {
    const Blister b = make_blister();
    ReconInputs in = sho_inputs(b);
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    in.ionizer_override = ionizer_from_xy(40.0, 30.0, 1000.0);
    for (double bad : {0.0, -1.0, nan, inf}) {
        in.deg_per_px_at = [bad](double, double) -> std::optional<double> { return bad; };
        REQUIRE_THROWS_AS(reconstruct(in, ReconConfig{}, gdg::Log{}), std::invalid_argument);
        REQUIRE_THROWS_WITH(reconstruct(in, ReconConfig{}, gdg::Log{}),
                            Catch::Matchers::ContainsSubstring("deg_per_px_at(40.0"));
    }
    in.deg_per_px_at = nullptr;
    for (double bad : {0.0, -5.0, nan, inf}) {
        in.ionizer_override = ionizer_from_xy(40.0, 30.0, bad);
        REQUIRE_THROWS_AS(reconstruct(in, ReconConfig{}, gdg::Log{}), std::invalid_argument);
        REQUIRE_THROWS_WITH(reconstruct(in, ReconConfig{}, gdg::Log{}),
                            Catch::Matchers::ContainsSubstring("distance_pc"));
    }
}

TEST_CASE("reconstruct_override_ionizer_used", "[recon][reconstruct]") {
    const Blister b = make_blister();
    ReconInputs in = sho_inputs(b);
    in.ionizer_override = ionizer_from_xy(40.0, 30.0, 500.0);
    StarRow s{10.0, 10.0, 100.0};
    s.bp_rp = -0.3;
    s.r_med = 900.0;
    in.stars = {s};  // present but must be ignored
    const ReconResult r = reconstruct(in, ReconConfig{}, gdg::Log{});
    CHECK(r.ionizer.source == "override");
    CHECK(r.model.meta.ionizer_source == "override");
    CHECK(r.model.apex_xy.first == 40.0);
    CHECK(r.model.apex_xy.second == 30.0);
}

TEST_CASE("reconstruct_pc_per_px", "[recon][reconstruct]") {
    const Blister b = make_blister();
    ReconInputs in = sho_inputs(b);
    in.ionizer_override.reset();
    StarRow s{60.0, 40.0, 100.0};
    s.bp_rp = -0.3;
    s.r_med = 1000.0;
    in.stars = {s};
    double qx = -1, qy = -1;
    in.deg_per_px_at = [&](double x, double y) -> std::optional<double> {
        qx = x;
        qy = y;
        return 1.0 / 3600.0;
    };
    const ReconResult r = reconstruct(in, ReconConfig{}, gdg::Log{});
    CHECK(r.ionizer.source == "gaia-blue");
    CHECK(qx == 60.0);
    CHECK(qy == 40.0);
    REQUIRE(r.model.pc_per_px.has_value());
    const double expect = 1000.0 * (1.0 / 3600.0) * PI / 180.0;
    CHECK(std::abs(*r.model.pc_per_px - expect) <= 1e-12 * expect);

    // astrometry that declines at the ionizer: no scale
    in.deg_per_px_at = [](double, double) -> std::optional<double> { return std::nullopt; };
    CHECK_FALSE(reconstruct(in, ReconConfig{}, gdg::Log{}).model.pc_per_px.has_value());
    // no astrometry at all: no scale
    in.deg_per_px_at = nullptr;
    CHECK_FALSE(reconstruct(in, ReconConfig{}, gdg::Log{}).model.pc_per_px.has_value());
}

TEST_CASE("reconstruct_no_distance_no_scale", "[recon][reconstruct]") {
    const Blister b = make_blister();
    ReconInputs in = sho_inputs(b);
    in.ionizer_override.reset();
    in.deg_per_px_at = [](double, double) -> std::optional<double> { return 1.0 / 3600.0; };
    const ReconResult r = reconstruct(in, ReconConfig{}, gdg::Log{});
    CHECK(r.ionizer.source == "field-center");
    CHECK(r.ionizer.x == b.ha.width / 2.0);
    CHECK(r.ionizer.y == b.ha.height / 2.0);
    CHECK_FALSE(r.model.pc_per_px.has_value());

    // an override without distance: also no scale
    in.ionizer_override = ionizer_from_xy(50.0, 40.0, std::nullopt);
    CHECK_FALSE(reconstruct(in, ReconConfig{}, gdg::Log{}).model.pc_per_px.has_value());
}

TEST_CASE("reconstruct_concurrent_calls_identical", "[recon][reconstruct]") {
    const Blister b = make_blister();
    const ReconInputs in = sho_inputs(b);
    const ReconResult serial = reconstruct(in, ReconConfig{}, gdg::Log{});
    std::optional<ReconResult> r1, r2;
    std::thread t1([&] { r1.emplace(reconstruct(in, ReconConfig{}, gdg::Log{})); });
    std::thread t2([&] { r2.emplace(reconstruct(in, ReconConfig{}, gdg::Log{})); });
    t1.join();
    t2.join();
    for (const auto* r : {&*r1, &*r2}) {
        CHECK(r->model.z.data == serial.model.z.data);
        CHECK(r->model.dust.data == serial.model.dust.data);
        CHECK(r->model.emissivity.data == serial.model.emissivity.data);
        CHECK(r->lum.data == serial.lum.data);
    }
}

TEST_CASE("reconstruct_concurrent_star_table_identical", "[recon][reconstruct]") {
    const Blister b = make_blister();
    ReconInputs in = sho_inputs(b);
    in.ionizer_override.reset();
    for (int i = 0; i < 6; ++i) {
        StarRow s{20.0 + 15.0 * i, 20.0 + 9.0 * i, 100.0 + i};
        s.bp_rp = 0.5 - 0.1 * i;
        s.r_med = 800.0 + 10.0 * i;
        in.stars.push_back(s);
    }
    in.deg_per_px_at = [](double, double) -> std::optional<double> { return 1.0 / 3600.0; };
    const ReconResult serial = reconstruct(in, ReconConfig{}, gdg::Log{});
    CHECK(serial.ionizer.source == "gaia-blue");
    std::optional<ReconResult> r1, r2;
    std::thread t1([&] { r1.emplace(reconstruct(in, ReconConfig{}, gdg::Log{})); });
    std::thread t2([&] { r2.emplace(reconstruct(in, ReconConfig{}, gdg::Log{})); });
    t1.join();
    t2.join();
    for (const auto* r : {&*r1, &*r2}) {
        CHECK(r->model.z.data == serial.model.z.data);
        CHECK(r->model.dust.data == serial.model.dust.data);
        CHECK(r->model.pc_per_px == serial.model.pc_per_px);
        CHECK(r->ionizer.x == serial.ionizer.x);
        CHECK(r->ionizer.y == serial.ionizer.y);
    }
}
