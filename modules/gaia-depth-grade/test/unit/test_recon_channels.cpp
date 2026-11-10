// Task 18 (recon plan task 2): channels.py -- SHO / duoband / OSC ingest and looks_linear,
// against the Task 1 goldens (make_goldens.py recon(), "Channel ingest on small arrays") plus the
// edge cases of gaia-depth-grade/tests/recon/test_channels.py.
#include <catch_amalgamated.hpp>

#include "ReconGolden.h"
#include "core/recon/Channels.h"

#include <cmath>
#include <random>
#include <stdexcept>

using namespace gdg::recon;
using gdg::Image;
using gdg::test::check_golden_image;
using gdg::test::golden_image;

namespace {

Image powered(const Image& in, double p) {
    Image out = in;
    for (double& v : out.data) v = std::pow(v, p);
    return out;
}

/// test_channels._linear_master: sky floor 0.002 + noise 0.0005, three Gaussian blobs, clipped at
/// 0, float32. numpy's PCG64 normal stream is replaced by std::mt19937_64 (the properties under
/// test do not depend on the exact noise).
Image linear_master(unsigned seed = 0, int n = 128) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> nd;
    Image img(n, n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double v = 0.002 + 0.0005 * nd(rng);
            for (auto [cy, cx, a] : {std::tuple{40, 40, 0.5}, std::tuple{90, 80, 0.3}, std::tuple{60, 100, 0.2}})
                v += a * std::exp(-((y - cy) * (y - cy) + (x - cx) * (x - cx)) / (2 * 8.0 * 8.0));
            img.at(x, y) = f32(std::max(v, 0.0));
        }
    return img;
}

/// test_channels._stretch: asinh(1000 x / max) / asinh(1000), float32.
Image stretch(const Image& in) {
    double mx = in.data[0];
    for (double v : in.data) mx = std::max(mx, v);
    Image out = in;
    for (double& v : out.data) v = f32(std::asinh(1000 * (v / mx)) / std::asinh(1000.0));
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

}  // namespace

TEST_CASE("channels_match_golden", "[recon][channels][golden]") {
    const Image ha = golden_image("chan_ha"), oiii = golden_image("chan_oiii"), sii = golden_image("chan_sii");
    // float32 pass-through and float32 min/subtract/average: bit-exact by construction.
    const ChannelSet sho = from_mono_sho(ha, oiii, sii);
    check_golden_image(sho.ha, "chan_out_sho_ha", 0.0, 0);
    check_golden_image(sho.oiii, "chan_out_sho_oiii", 0.0, 0);
    REQUIRE(sho.sii);
    check_golden_image(*sho.sii, "chan_out_sho_sii", 0.0, 0);
    CHECK(sho.ratio_confidence == 1.0);
    CHECK(sho.is_linear);

    const ChannelSet duo = from_duoband(golden_image("chan_d1"), golden_image("chan_d2"));
    check_golden_image(duo.ha, "chan_out_duoband_ha", 0.0, 0);
    check_golden_image(duo.oiii, "chan_out_duoband_oiii", 0.0, 0);
    REQUIRE(duo.sii);
    check_golden_image(*duo.sii, "chan_out_duoband_sii", 0.0, 0);
    CHECK(duo.ratio_confidence == 0.6);
    CHECK(duo.is_linear);

    const ChannelSet osc = from_osc_rgb(golden_image("chan_rgb"));
    check_golden_image(osc.ha, "chan_out_osc_ha", 0.0, 0);
    check_golden_image(osc.oiii, "chan_out_osc_oiii", 0.0, 0);
    CHECK_FALSE(osc.sii);
    CHECK(osc.ratio_confidence == 0.25);
    CHECK(osc.is_linear);
}

TEST_CASE("looks_linear_matches_golden", "[recon][channels][golden]") {
    // stage.json "looks_linear": linear_ha true, stretched_ha false, flat true, ramp false.
    const Image ha = golden_image("chan_ha");
    CHECK(looks_linear(ha));
    CHECK_FALSE(looks_linear(powered(ha, 0.3)));
    CHECK(looks_linear(Image(24, 18, 1, 0.2)));
    Image ramp(25, 20);
    for (int i = 0; i < 500; ++i) ramp.data[i] = i / 499.0;  // np.linspace(0, 1, 500)
    CHECK_FALSE(looks_linear(ramp));
    CHECK(LINEAR_MEDIAN_FRAC_MAX == 0.10);
}

TEST_CASE("extract_channels_stretched_matches_golden", "[recon][channels][golden]") {
    // stage.json "chan_extract_stretched": ratio_confidence 0.0, is_linear false.
    const ChannelSet ex = extract_channels({{"ha", powered(golden_image("chan_ha"), 0.3)},
                                            {"oiii", powered(golden_image("chan_oiii"), 0.3)},
                                            {"sii", powered(golden_image("chan_sii"), 0.3)}},
                                           "sho");
    CHECK(ex.ratio_confidence == 0.0);
    CHECK_FALSE(ex.is_linear);
}

TEST_CASE("sho_passthrough_full_confidence", "[recon][channels]") {
    const Image ha = linear_master(1), oiii = linear_master(2), sii = linear_master(3);
    const ChannelSet cs = from_mono_sho(ha, oiii, sii);
    CHECK(cs.ha.data == ha.data);
    CHECK(cs.oiii.data == oiii.data);
    REQUIRE(cs.sii);
    CHECK(cs.sii->data == sii.data);
    CHECK(cs.ratio_confidence == 1.0);
    CHECK(cs.is_linear);
    // float32 cast of a non-float32 input
    const ChannelSet c2 = from_mono_sho(Image(2, 2, 1, 0.1), Image(2, 2), Image(2, 2));
    CHECK(c2.ha.data[0] == static_cast<double>(0.1f));
}

TEST_CASE("duoband_unmix_recovers_planted_lines", "[recon][channels]") {
    const int n = 64;
    Image d1(n, n), d2(n, n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double ha = (y < 32 && x < 32) ? 0.8 : 0.0, sii = (y >= 32 && x >= 32) ? 0.5 : 0.0;
            d1.at(x, y) = static_cast<float>(ha) + 0.3f;
            d2.at(x, y) = static_cast<float>(sii) + 0.3f;
        }
    const ChannelSet cs = from_duoband(d1, d2);
    CHECK(cs.ratio_confidence == 0.6);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            CHECK(std::fabs(cs.oiii.at(x, y) - 0.3) <= 0.03);
            if (y < 32 && x < 32) CHECK(std::fabs(cs.ha.at(x, y) - 0.8) <= 0.08);
            if (y >= 32 && x >= 32) {
                CHECK(std::fabs(cs.sii->at(x, y) - 0.5) <= 0.05);
                CHECK(cs.ha.at(x, y) < 0.03);
            }
            if (y < 32 && x < 32) CHECK(cs.sii->at(x, y) < 0.03);
        }
}

TEST_CASE("osc_proxy_maps_channels", "[recon][channels]") {
    std::mt19937_64 rng(5);
    std::uniform_real_distribution<double> u;
    Image rgb(20, 16, 3);
    for (double& v : rgb.data) v = f32(u(rng));
    const ChannelSet cs = from_osc_rgb(rgb);
    for (size_t i = 0; i < rgb.pixels(); ++i) {
        CHECK(cs.ha.data[i] == rgb.data[3 * i]);
        const float s = static_cast<float>(rgb.data[3 * i + 1]) + static_cast<float>(rgb.data[3 * i + 2]);
        CHECK(cs.oiii.data[i] == static_cast<double>(0.5f * s));
    }
    CHECK_FALSE(cs.sii);
    CHECK(cs.ratio_confidence == 0.25);
    CHECK_THROWS_AS(from_osc_rgb(Image(20, 16, 1)), std::invalid_argument);
    CHECK_THROWS_AS(from_osc_rgb(Image(20, 16, 4)), std::invalid_argument);
}

TEST_CASE("mismatched_shapes_raise", "[recon][channels]") {
    const Image a = linear_master(1, 64), b = linear_master(2, 64), c = linear_master(3, 48);
    CHECK_THROWS_AS(extract_channels({{"ha", a}, {"oiii", b}, {"sii", c}}, "sho"), std::invalid_argument);
    CHECK_THROWS_AS(extract_channels({{"d1", a}, {"d2", c}}, "duoband"), std::invalid_argument);
    CHECK_THROWS_AS(from_mono_sho(a, b, c), std::invalid_argument);
    CHECK_THROWS_AS(from_duoband(a, c), std::invalid_argument);
}

TEST_CASE("looks_linear_discriminates", "[recon][channels]") {
    CHECK(looks_linear(linear_master()));
    CHECK_FALSE(looks_linear(stretch(linear_master())));
}

TEST_CASE("stretched_input_zeros_ratio_confidence", "[recon][channels]") {
    const ChannelSet cs = extract_channels(
        {{"ha", stretch(linear_master(1))}, {"oiii", stretch(linear_master(2))}, {"sii", stretch(linear_master(3))}},
        "sho");
    CHECK_FALSE(cs.is_linear);
    CHECK(cs.ratio_confidence == 0.0);
    // the planes are kept
    CHECK(cs.ha.data == stretch(linear_master(1)).data);
    CHECK(cs.sii);
    // one stretched probe is enough
    const ChannelSet one = extract_channels(
        {{"ha", linear_master(1)}, {"oiii", linear_master(2)}, {"sii", stretch(linear_master(3))}}, "sho");
    CHECK_FALSE(one.is_linear);
    CHECK(one.ratio_confidence == 0.0);
}

TEST_CASE("linear_input_keeps_confidence_and_modes", "[recon][channels]") {
    const Image m1 = linear_master(1), m2 = linear_master(2), m3 = linear_master(3);
    CHECK(extract_channels({{"ha", m1}, {"oiii", m2}, {"sii", m3}}, "sho").ratio_confidence == 1.0);
    const ChannelSet d = extract_channels({{"d1", m1}, {"d2", m2}}, "duoband");
    CHECK(d.is_linear);
    CHECK(d.ratio_confidence == 0.6);
    const Image rgb = stack3(m1, m2, m3);
    const ChannelSet o = extract_channels({{"rgb", rgb}}, "osc");
    CHECK_FALSE(o.sii);
    CHECK(o.ratio_confidence == 0.25);
    CHECK_THROWS_WITH(extract_channels({{"rgb", rgb}}, "bogus"),
                      "unknown mode 'bogus'; expected one of ['duoband', 'osc', 'sho']");
    CHECK_THROWS_WITH(extract_channels({{"ha", m1}}, "sho"), "mode 'sho' missing inputs: ['oiii', 'sii']");
    // osc probes each RGB channel: one stretched channel zeroes the confidence
    const ChannelSet os = extract_channels({{"rgb", stack3(m1, m2, stretch(m3))}}, "osc");
    CHECK_FALSE(os.is_linear);
    CHECK(os.ratio_confidence == 0.0);
}
