// Task 21 (Phase G2): render_cavity_2d -- the CavityModel as a registered linear RGB image -- and
// the C++ synthetic blister (test/util/Blister.h) Tasks 22-24 reuse. No Python goldens: every
// assertion is analytic truth on the blister or an exact invariant of the render formula.
#include <catch_amalgamated.hpp>

#include "Blister.h"
#include "core/Numerics.h"
#include "core/recon/Cavity.h"
#include "core/recon/Channels.h"
#include "core/recon/Extinction.h"
#include "core/recon/Ionization.h"
#include "core/recon/Ionizer.h"
#include "core/recon/Render2D.h"

#include <cmath>
#include <stdexcept>
#include <thread>

using namespace gdg::recon;
using gdg::Image;
using gdg::test::Blister;
using gdg::test::make_blister;

namespace {

Image sum3(const Image& a, const Image& b, const Image& c) {
    Image out = a;
    for (size_t i = 0; i < out.data.size(); ++i) out.data[i] = a.data[i] + b.data[i] + c.data[i];
    return out;
}

/// The blister through the recon core: SHO channels, an override ionizer at the truth apex,
/// lum = ha + oiii + sii, dust from the luminance structure.
CavityModel blister_model(const Blister& b) {
    const ChannelSet ch = from_mono_sho(b.ha, b.oiii, b.sii);
    const Image lum = sum3(b.ha, b.oiii, b.sii);
    return reconstruct_cavity(ch, ionization_ratio(ch), ionizer_from_xy(b.truth.apex_x, b.truth.apex_y, std::nullopt),
                              lum, dust_from_structure(lum), std::nullopt, ReconConfig{});
}

/// Mean of the per-pixel render transfer sum_c(out) / sum_c(E) over pixels whose distance from
/// the truth apex lies in [r0, r1] (in units of R) and whose emissivity is non-zero.
double mean_transfer(const Image& out, const Image& e, const gdg::test::BlisterTruth& t, double r0, double r1) {
    double s = 0.0;
    size_t n = 0;
    for (int y = 0; y < e.height; ++y)
        for (int x = 0; x < e.width; ++x) {
            const double r = std::hypot(x - t.apex_x, y - t.apex_y) / t.radius_px;
            if (r < r0 || r > r1) continue;
            double so = 0.0, se = 0.0;
            for (int c = 0; c < 3; ++c) {
                so += out.at(x, y, c);
                se += e.at(x, y, c);
            }
            if (se <= 1e-6) continue;
            s += so / se;
            ++n;
        }
    REQUIRE(n > 0);
    return s / static_cast<double>(n);
}

/// The limb term's g, computed independently of Render2D.cpp: |np.gradient(gaussian_filter(z, 1))|
/// written out with numpy's index arithmetic (central inside, one-sided at the edges).
Image gradient_magnitude(const Image& z) {
    const Image s = gdg::gaussian_filter(z, 1.0);
    const int nx = s.width, ny = s.height;
    Image g(nx, ny);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            const double gx = x == 0        ? s.at(1, y) - s.at(0, y)
                              : x == nx - 1 ? s.at(nx - 1, y) - s.at(nx - 2, y)
                                            : (s.at(x + 1, y) - s.at(x - 1, y)) / 2.0;
            const double gy = y == 0        ? s.at(x, 1) - s.at(x, 0)
                              : y == ny - 1 ? s.at(x, ny - 1) - s.at(x, ny - 2)
                                            : (s.at(x, y + 1) - s.at(x, y - 1)) / 2.0;
            g.at(x, y) = std::sqrt(gx * gx + gy * gy);
        }
    return g;
}

}  // namespace

TEST_CASE("blister_is_deterministic", "[recon][render2d][blister]") {
    const Blister a = make_blister(), b = make_blister();
    CHECK(a.ha.data == b.ha.data);
    CHECK(a.oiii.data == b.oiii.data);
    CHECK(a.sii.data == b.sii.data);
    CHECK(a.rgb.data == b.rgb.data);
    // a different seed changes only the noise
    const Blister c = make_blister(128, 96, 7);
    CHECK(c.ha.data != a.ha.data);
    double worst = 0.0;
    for (size_t i = 0; i < a.ha.data.size(); ++i) worst = std::max(worst, std::fabs(c.ha.data[i] - a.ha.data[i]));
    CHECK(worst < 0.01);  // 2 x 0.0005 sigma noise, never more than ~10 sigma apart

    // shape and the documented truth
    CHECK(a.ha.width == 128);
    CHECK(a.ha.height == 96);
    CHECK(a.ha.channels == 1);
    CHECK(a.rgb.channels == 3);
    CHECK(a.truth.apex_x == 0.55 * 128);
    CHECK(a.truth.apex_y == 0.45 * 96);
    CHECK(a.truth.radius_px == 0.4 * 96);
    CHECK(a.truth.lane_y1 - a.truth.lane_y0 == 3.0);  // rows lane_y0..lane_y1 inclusive: 4 px
    // rgb = (ha + sii, 0.5 oiii, oiii), float32-valued like the planes
    for (size_t i = 0; i < a.ha.pixels(); ++i) {
        REQUIRE(a.rgb.data[3 * i] == f32(a.ha.data[i] + a.sii.data[i]));
        REQUIRE(a.rgb.data[3 * i + 1] == 0.5 * a.oiii.data[i]);
        REQUIRE(a.rgb.data[3 * i + 2] == a.oiii.data[i]);
    }
    // [O III] dominates the core, H-alpha the rim, the lane is dark, the sky sits at 0.002
    const auto& t = a.truth;
    const int ax = static_cast<int>(std::lround(t.apex_x)), ay = static_cast<int>(std::lround(t.apex_y));
    CHECK(a.oiii.at(ax, ay) > 10.0 * a.ha.at(ax, ay));
    const int rim_x = static_cast<int>(std::lround(t.apex_x - 0.8 * t.radius_px));
    CHECK(a.ha.at(rim_x, ay) > a.oiii.at(rim_x, ay));
    const int lane_y = static_cast<int>(t.lane_y0) + 1, lane_x = static_cast<int>(std::lround(t.apex_x - 0.75 * t.radius_px));
    const double open = a.ha.at(lane_x, ay), lane = a.ha.at(lane_x, lane_y);
    CHECK(lane < 0.3 * open);
    CHECK(a.ha.at(0, 0) == Catch::Approx(0.002).margin(0.0025));  // 5 sigma of the 0.0005 noise
}

TEST_CASE("render2d_output_registered_linear_rgb", "[recon][render2d]") {
    const Blister b = make_blister();
    const CavityModel m = blister_model(b);
    const Image out = render_cavity_2d(m, ReconConfig{});
    CHECK(out.width == b.ha.width);
    CHECK(out.height == b.ha.height);
    CHECK(out.channels == 3);
    for (double v : out.data) {
        REQUIRE(std::isfinite(v));
        REQUIRE(v >= 0.0);
        REQUIRE(v <= 1.0);
        REQUIRE(v == f32(v));
    }
}

TEST_CASE("render2d_core_reads_concave", "[recon][render2d]") {
    const Blister b = make_blister();
    const CavityModel m = blister_model(b);
    const Image out = render_cavity_2d(m, ReconConfig{});
    const double core = mean_transfer(out, m.emissivity, b.truth, 0.0, 0.1);
    const double rim = mean_transfer(out, m.emissivity, b.truth, 0.8, 1.0);
    INFO("core " << core << "  rim " << rim);
    CHECK(core <= rim - 0.15);
}

TEST_CASE("render2d_dust_occludes", "[recon][render2d]") {
    const Blister b = make_blister();
    const CavityModel m = blister_model(b);
    size_t dusty = 0;
    for (double d : m.dust.data) dusty += d >= 0.5 ? 1 : 0;
    REQUIRE(dusty > 0);  // the lane is dust

    // Ruling R1: with the limb term off, shade <= 1, so out <= 0.5 E is exact where dust >= 0.5.
    ReconConfig no_limb;
    no_limb.render_limb_gain = 0.0;
    no_limb.render_dust_strength = 1.0;
    const Image o0 = render_cavity_2d(m, no_limb);
    // With the default limb gain the bound grows by the limb factor's ceiling, 1 + gain.
    ReconConfig deflt;
    deflt.render_dust_strength = 1.0;
    const Image o1 = render_cavity_2d(m, deflt);
    const double lim1 = 0.5 * (1.0 + deflt.render_limb_gain);
    for (size_t i = 0; i < m.dust.pixels(); ++i) {
        if (m.dust.data[i] < 0.5) continue;
        for (int c = 0; c < 3; ++c) {
            const double e = m.emissivity.data[3 * i + c];
            REQUIRE(o0.data[3 * i + c] <= 0.5 * e);
            REQUIRE(o1.data[3 * i + c] <= lim1 * e);
        }
    }
    // dust strength 0 switches occlusion off: the dusty pixels brighten
    ReconConfig clear = no_limb;
    clear.render_dust_strength = 0.0;
    const Image oc = render_cavity_2d(m, clear);
    double dim = 0.0, lit = 0.0;
    for (size_t i = 0; i < m.dust.pixels(); ++i)
        if (m.dust.data[i] >= 0.5)
            for (int c = 0; c < 3; ++c) {
                dim += o0.data[3 * i + c];
                lit += oc.data[3 * i + c];
            }
    CHECK(dim < 0.5 * lit + 1e-12);
}

TEST_CASE("render2d_limb_brightens_depth_edges", "[recon][render2d]") {
    const Blister b = make_blister();
    const CavityModel m = blister_model(b);
    ReconConfig off;
    off.render_limb_gain = 0.0;
    ReconConfig on;
    on.render_limb_gain = 0.5;
    const Image o0 = render_cavity_2d(m, off), o5 = render_cavity_2d(m, on);
    const Image g = gradient_magnitude(m.z);
    const auto& t = b.truth;

    // Ceiling of the factor: 1 + gain, plus one float32 rounding of each output.
    const double ceiling = (1.0 + on.render_limb_gain) * (1.0 + 2.0 * 0x1p-23);
    size_t flat = 0, edge = 0;
    for (int y = 0; y < m.z.height; ++y)
        for (int x = 0; x < m.z.width; ++x) {
            const double rho = std::hypot(x - t.apex_x, y - t.apex_y) / t.radius_px;
            const bool at_edge = g.at(x, y) > 0.0 && rho >= 0.9 && rho <= 1.1;
            if (g.at(x, y) == 0.0) ++flat;
            for (int c = 0; c < 3; ++c) {
                const double a = o0.at(x, y, c), v = o5.at(x, y, c);
                if (g.at(x, y) == 0.0) {
                    REQUIRE(v == a);  // no gradient: the limb term is exactly 1
                    continue;
                }
                REQUIRE(v >= a);  // the limb factor never darkens
                if (a > 0.0) REQUIRE(v <= ceiling * a);
                if (at_edge && m.emissivity.at(x, y, c) > 1e-6) {
                    REQUIRE(a < 1.0);  // unclipped, so the factor is visible
                    REQUIRE(v > a);
                    ++edge;
                }
            }
        }
    INFO("flat px " << flat << ", edge samples " << edge);
    CHECK(flat > 0);  // the sky beyond the smoothed footprint edge has g == 0
    CHECK(edge > 0);
}

TEST_CASE("render2d_degenerate_is_identity", "[recon][render2d]") {
    const Blister b = make_blister();
    const CavityModel src = blister_model(b);
    Image e = src.emissivity;
    e.data[0] = 1.7;  // out-of-range emissivity: the identity is clip(E, 0, 1)
    e.data[1] = -0.2;
    const CavityModel flat(Image(b.ha.width, b.ha.height, 1, 0.3), e, Image(b.ha.width, b.ha.height), std::nullopt,
                           src.apex_xy, src.meta);
    for (const ReconConfig& cfg : {ReconConfig{}, [] {
                                       ReconConfig c;
                                       c.render_gamma = 2.2;
                                       c.render_ambient = 0.3;
                                       c.render_depth_shading = 1.5;
                                       return c;
                                   }()}) {
        const Image out = render_cavity_2d(flat, cfg);
        double worst = 0.0;
        for (size_t i = 0; i < out.data.size(); ++i)
            worst = std::max(worst, std::fabs(out.data[i] - clip(e.data[i], 0.0, 1.0)));
        CHECK(worst <= 1e-7);
    }
}

TEST_CASE("render2d_rejects_mono_emissivity", "[recon][render2d]") {
    const Image z(8, 6), dust(8, 6), mono(8, 6), two(8, 6, 2), rgb(8, 6, 3);
    const CavityMeta meta;
    CHECK_THROWS_AS(render_cavity_2d(CavityModel(z, mono, dust, std::nullopt, {4.0, 3.0}, meta), ReconConfig{}),
                    std::invalid_argument);
    CHECK_THROWS_AS(render_cavity_2d(CavityModel(z, two, dust, std::nullopt, {4.0, 3.0}, meta), ReconConfig{}),
                    std::invalid_argument);
    // z and dust must be (ny, nx) planes
    CHECK_THROWS_AS(render_cavity_2d(CavityModel(rgb, rgb, dust, std::nullopt, {4.0, 3.0}, meta), ReconConfig{}),
                    std::invalid_argument);
    CHECK_THROWS_AS(render_cavity_2d(CavityModel(z, rgb, rgb, std::nullopt, {4.0, 3.0}, meta), ReconConfig{}),
                    std::invalid_argument);
    CHECK_NOTHROW(render_cavity_2d(CavityModel(z, rgb, dust, std::nullopt, {4.0, 3.0}, meta), ReconConfig{}));
}

TEST_CASE("render2d_concurrent_calls_identical", "[recon][render2d]") {
    const Blister b = make_blister();
    const CavityModel m = blister_model(b);
    const Image serial = render_cavity_2d(m, ReconConfig{});
    Image o1, o2;
    std::thread t1([&] { o1 = render_cavity_2d(m, ReconConfig{}); });
    std::thread t2([&] { o2 = render_cavity_2d(m, ReconConfig{}); });
    t1.join();
    t2.join();
    CHECK(o1.data == serial.data);
    CHECK(o2.data == serial.data);
}

TEST_CASE("render2d_rejects_mismatched_plane_shapes", "[recon][render2d]") {
    // A plane replaced after construction (the members are public) is refused before any pixel is
    // read: a narrower plane would otherwise be indexed out of bounds.
    const Image z(8, 6), dust(8, 6), rgb(8, 6, 3);
    for (const Image& bad : {Image(7, 6), Image(9, 6), Image(8, 5)}) {
        CavityModel m(z, rgb, dust, std::nullopt, {4.0, 3.0}, CavityMeta{});
        m.dust = bad;
        CHECK_THROWS_WITH(render_cavity_2d(m, ReconConfig{}), Catch::Matchers::ContainsSubstring("shape mismatch"));
        CavityModel e(z, rgb, dust, std::nullopt, {4.0, 3.0}, CavityMeta{});
        e.emissivity = Image(bad.width, bad.height, 3);
        CHECK_THROWS_WITH(render_cavity_2d(e, ReconConfig{}), Catch::Matchers::ContainsSubstring("shape mismatch"));
    }
}
