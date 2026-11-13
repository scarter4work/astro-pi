// Task 18 (recon plan task 1): model.py types -- shape validation and the ReconConfig defaults.
// Edge cases ported 1:1 from gaia-depth-grade/tests/recon/test_model.py.
#include <catch_amalgamated.hpp>

#include "core/recon/Model.h"

#include <stdexcept>

using namespace gdg::recon;
using gdg::Image;

TEST_CASE("channelset_rejects_shape_mismatch", "[recon][model]") {
    const Image a(4, 4), b(5, 4);
    CHECK_THROWS_AS(ChannelSet(a, b, std::nullopt, 1.0, true), std::invalid_argument);
    // sii takes part in the check too
    CHECK_THROWS_AS(ChannelSet(a, a, Image(4, 5), 1.0, true), std::invalid_argument);
    CHECK_NOTHROW(ChannelSet(a, a, std::nullopt, 1.0, true));
    CHECK_NOTHROW(ChannelSet(a, a, Image(4, 4), 1.0, true));
}

TEST_CASE("cavitymodel_fields_roundtrip", "[recon][model]") {
    const int ny = 6, nx = 8;
    CavityMeta meta;
    meta.ionizer_source = "test";
    const CavityModel m(Image(nx, ny), Image(nx, ny, 3), Image(nx, ny), 2.5, {3.0, 4.0}, meta);
    CHECK(m.z.height == ny);
    CHECK(m.z.width == nx);
    CHECK(m.emissivity.channels == 3);
    CHECK(m.dust.width == nx);
    CHECK(m.pc_per_px == 2.5);
    CHECK(m.apex_xy == std::pair<double, double>{3.0, 4.0});
    CHECK(m.meta.ionizer_source == "test");
}

TEST_CASE("cavitymodel_rejects_emissivity_shape_mismatch", "[recon][model]") {
    const int ny = 6, nx = 8;
    CHECK_THROWS_AS(CavityModel(Image(nx, ny), Image(nx + 1, ny, 3), Image(nx, ny), std::nullopt, {0.0, 0.0}, {}),
                    std::invalid_argument);
}

TEST_CASE("validate_shapes_compares_leading_shape_only", "[recon][model]") {
    const Image mono(8, 6), rgb(8, 6, 3), other(8, 7);
    CHECK_NOTHROW(validate_shapes({&mono, &rgb, nullptr}));
    CHECK_NOTHROW(validate_shapes({}));
    CHECK_THROWS_WITH(validate_shapes({&mono, &other}), "array shape mismatch: [(6, 8), (7, 8)]");
}

TEST_CASE("reconconfig_defaults_verbatim", "[recon][model]") {
    const ReconConfig c;
    CHECK(c.w_geometry == 1.0);
    CHECK(c.w_ionization == 0.6);
    CHECK(c.w_emission == 0.15);
    CHECK(c.emission_sign == 1.0);
    CHECK(c.w_dust == 0.8);
    CHECK(c.footprint_frac == 0.05);
    CHECK(c.geometry_power == 2.0);
    CHECK(c.ionization_gamma == 1.0);
    CHECK(c.render_gamma == 1.0);
    CHECK(c.render_ambient == 0.05);
    CHECK(c.render_depth_shading == 0.5);
    CHECK(c.render_dust_strength == 1.0);
    CHECK(c.render_limb_gain == 0.5);
}

TEST_CASE("numpy_scalar_helpers", "[recon][model]") {
    CHECK(f32(0.1) == static_cast<double>(0.1f));
    CHECK(nan_to_num(std::nan("")) == 0.0);
    CHECK(nan_to_num(INFINITY) == 1.7976931348623157e308);
    CHECK(nan_to_num(-INFINITY) == -1.7976931348623157e308);
    CHECK(nan_to_num(-2.5) == -2.5);
    CHECK(clip(-1.0, 0.0, 1.0) == 0.0);
    CHECK(clip(2.0, 0.0, 1.0) == 1.0);
    CHECK(clip(0.25, 0.0, 1.0) == 0.25);
    Mask m(3, 2);
    CHECK_FALSE(m.any());
    m.data[4] = 1;
    CHECK(m.any());
    CHECK(m.count() == 1u);
}
