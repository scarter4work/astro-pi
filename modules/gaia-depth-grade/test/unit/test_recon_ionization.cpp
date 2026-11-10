// Task 18 (recon plan task 3): ionization.py against the synthetic_blister golden, plus the edge
// cases of gaia-depth-grade/tests/recon/test_ionization.py.
#include <catch_amalgamated.hpp>

#include "ReconGolden.h"
#include "core/recon/Ionization.h"

#include <cmath>

using namespace gdg::recon;
using gdg::Image;

TEST_CASE("ionization_ratio_matches_golden", "[recon][ionization][golden]") {
    // gaussian_filter is scipy-exact and the rest is one IEEE division and a clip per pixel.
    const IonizationMap m = ionization_ratio(gdg::test::blister_channels());
    gdg::test::check_golden_image(m.oiii_fraction, "recon_oiii_fraction", 0.0, 0);
    CHECK(m.confidence == 1.0);  // stage.json recon_ionization_ratio
    CHECK(IONIZATION_SMOOTH_SIGMA_PX == 2.0);
}

namespace {

struct Disc {
    Image ha, oiii;
    std::vector<int> core, ring;  // pixel indices
};

Disc disc() {
    const int n = 64;
    Disc d{Image(n, n, 1, static_cast<float>(0.01)), Image(n, n, 1, static_cast<float>(0.01)), {}, {}};
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double r = std::hypot(y - 32.0, x - 32.0);
            const int i = y * n + x;
            if (r < 10) {
                d.oiii.data[i] = 1.0;
                d.ha.data[i] = static_cast<float>(0.1);
                d.core.push_back(i);
            } else if (r >= 18 && r < 26) {
                d.ha.data[i] = 1.0;
                d.oiii.data[i] = static_cast<float>(0.05);
                d.ring.push_back(i);
            }
        }
    return d;
}

double mean_at(const Image& im, const std::vector<int>& idx) {
    double s = 0;
    for (int i : idx) s += im.data[i];
    return s / static_cast<double>(idx.size());
}

void check_unit_range_finite(const Image& f) {
    for (double v : f.data) {
        REQUIRE(std::isfinite(v));
        REQUIRE(v >= 0.0);
        REQUIRE(v <= 1.0);
        REQUIRE(v == f32(v));  // float32-valued
    }
}

}  // namespace

TEST_CASE("oiii_bright_core_reads_high_fraction", "[recon][ionization]") {
    const Disc d = disc();
    const IonizationMap m = ionization_ratio(ChannelSet(d.ha, d.oiii, std::nullopt, 0.8, true));
    CHECK(m.oiii_fraction.at(32, 32) > 0.7);
    CHECK(mean_at(m.oiii_fraction, d.core) > 0.7);
}

TEST_CASE("ha_dominant_edge_reads_low_fraction", "[recon][ionization]") {
    const Disc d = disc();
    const IonizationMap m = ionization_ratio(ChannelSet(d.ha, d.oiii, std::nullopt, 0.8, true));
    CHECK(mean_at(m.oiii_fraction, d.ring) < 0.3);
}

TEST_CASE("handles_sii_none_and_present", "[recon][ionization]") {
    const Disc d = disc();
    const IonizationMap none = ionization_ratio(ChannelSet(d.ha, d.oiii, std::nullopt, 0.8, true));
    CHECK(none.oiii_fraction.width == 64);
    CHECK(none.oiii_fraction.height == 64);
    check_unit_range_finite(none.oiii_fraction);
    const IonizationMap with = ionization_ratio(ChannelSet(d.ha, d.oiii, Image(64, 64, 1, 0.3), 0.8, true));
    check_unit_range_finite(with.oiii_fraction);
    CHECK(with.oiii_fraction.data == none.oiii_fraction.data);  // sii does not alter the map
}

TEST_CASE("confidence_propagates", "[recon][ionization]") {
    const Disc d = disc();
    CHECK(ionization_ratio(ChannelSet(d.ha, d.oiii, std::nullopt, 0.37, true)).confidence == 0.37);
}

TEST_CASE("zero_signal_is_finite", "[recon][ionization]") {
    const Image z(16, 16);
    const Image f = ionization_ratio(ChannelSet(z, z, std::nullopt, 0.5, true)).oiii_fraction;
    for (double v : f.data) CHECK(v == 0.0);
}

TEST_CASE("nan_and_negative_input_are_cleaned", "[recon][ionization]") {
    // nan_to_num then clip(., 0): NaN and negatives count as 0 flux.
    Image ha(8, 8, 1, 0.5), oiii(8, 8, 1, 0.5);
    ha.data[10] = std::nan("");
    ha.data[30] = -2.0;
    oiii.data[20] = -3.0;
    Image ha0 = ha, oiii0 = oiii;
    ha0.data[10] = 0.0;
    ha0.data[30] = 0.0;
    oiii0.data[20] = 0.0;
    const Image a = ionization_ratio(ChannelSet(ha, oiii, std::nullopt, 1.0, true)).oiii_fraction;
    const Image b = ionization_ratio(ChannelSet(ha0, oiii0, std::nullopt, 1.0, true)).oiii_fraction;
    CHECK(a.data == b.data);
    check_unit_range_finite(a);
}
