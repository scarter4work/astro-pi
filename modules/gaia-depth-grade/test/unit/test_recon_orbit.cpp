// Task 24 (Phase G2): the parallax orbit animation. Analytic splat checks on hand-built models,
// the C++ synthetic blister (test/util/Blister.h) end to end, and the GIF decoded by the test-only
// reader test/util/GifRead.h.
#include <catch_amalgamated.hpp>

#include "Blister.h"
#include "GifRead.h"
#include "GlbRead.h"
#include "TestFs.h"
#include "core/Display.h"
#include "core/recon/Glb.h"
#include "core/recon/Orbit.h"
#include "core/recon/Reconstruct.h"
#include "core/recon/Render2D.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace gdg::recon;
using gdg::Image;
using gdg::test::GifFile;
using gdg::test::no_temps;
using gdg::test::parse_gif;
using gdg::test::TempDir;
using gdg::test::thrown_message;

namespace {

namespace fs = std::filesystem;
constexpr double PI = 3.14159265358979323846;

CavityModel blister_model() {
    const gdg::test::Blister b = gdg::test::make_blister();
    ReconInputs in;
    in.mode = "sho";
    in.channels = {{"ha", b.ha}, {"oiii", b.oiii}, {"sii", b.sii}};
    in.ionizer_override = ionizer_from_xy(b.truth.apex_x, b.truth.apex_y, std::nullopt);
    return reconstruct(in, ReconConfig{}, gdg::Log{}).model;
}

/// An nx x ny model with z from `zf`, a distinct emissivity per pixel, no dust.
template <typename Z>
CavityModel plain_model(int nx, int ny, std::pair<double, double> apex, Z zf) {
    Image z(nx, ny), e(nx, ny, 3), d(nx, ny);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            z.at(x, y) = zf(x, y);
            e.at(x, y, 0) = 0.05 + 0.01 * x + 0.003 * y;
            e.at(x, y, 1) = 0.30 + 0.02 * y - 0.001 * x;
            e.at(x, y, 2) = 0.10 + 0.0007 * x * y;
        }
    CavityMeta meta;
    meta.capture_mode = "sho";
    meta.ionizer_source = "override";
    return CavityModel(std::move(z), std::move(e), std::move(d), std::nullopt, apex, meta);
}

double mean_abs_diff(const Image& a, const Image& b) {
    REQUIRE(a.same_shape(b));
    double s = 0.0;
    for (size_t i = 0; i < a.data.size(); ++i) s += std::abs(a.data[i] - b.data[i]);
    return s / static_cast<double>(a.data.size());
}

/// A 4-frame orbit whose frame 1 (theta = +amplitude) shifts a pixel by (z - z_apex) to within
/// 1e-12 px: depth_ratio = 1 / (R tan(amplitude)).
struct UnitShift {
    ExportConfig ecfg;
    OrbitConfig ocfg;
};
UnitShift unit_shift(const CavityModel& m) {
    UnitShift u;
    u.ocfg.frames = 4;
    u.ocfg.amplitude_deg = 10.0;
    u.ecfg.depth_ratio = 1.0 / (cavity_radius_px(m) * std::tan(10.0 * PI / 180.0));
    return u;
}

void check_pixel(const Image& got, int x, int y, const Image& base, int sx, int sy) {
    for (int c = 0; c < 3; ++c) {
        INFO("target (" << x << ", " << y << ") channel " << c << " from source (" << sx << ", " << sy << ")");
        CHECK(got.at(x, y, c) == base.at(sx, sy, c));
    }
}

void check_lerp(const Image& got, int x, int y, const Image& base, int lx, int rx, double w) {
    for (int c = 0; c < 3; ++c) {
        const double want = base.at(lx, y, c) + (base.at(rx, y, c) - base.at(lx, y, c)) * w;
        INFO("hole (" << x << ", " << y << ") channel " << c);
        CHECK(std::abs(got.at(x, y, c) - want) <= 1e-15);
    }
}

}  // namespace

TEST_CASE("orbit_frame0_equals_render", "[recon][orbit]") {
    const CavityModel m = blister_model();
    for (const ReconConfig& rc : {ReconConfig{}, [] {
                                      ReconConfig c;
                                      c.render_limb_gain = 0.0;
                                      c.render_depth_shading = 0.9;
                                      return c;
                                  }()}) {
        const Image base = render_cavity_2d(m, rc);
        const std::vector<Image> views = orbit_views(m, rc, ExportConfig{}, OrbitConfig{});
        REQUIRE(views.size() == 36);
        REQUIRE(views[0].same_shape(base));
        CHECK(views[0].data == base.data);  // bit-identical
        for (const Image& v : views) CHECK(v.same_shape(base));
    }
}

TEST_CASE("orbit_frames_differ", "[recon][orbit]") {
    const CavityModel m = blister_model();
    const OrbitConfig o;
    const std::vector<Image> v = orbit_views(m, ReconConfig{}, ExportConfig{}, o);
    const double d_quarter = mean_abs_diff(v[0], v[static_cast<size_t>(o.frames / 4)]);
    CAPTURE(d_quarter);
    CHECK(d_quarter > 1e-3);
    // The opposite yaws (frames 9 and 27: +12 and -12 degrees) are different views.
    CHECK(mean_abs_diff(v[9], v[27]) > 1e-3);
    // Every view keeps the render's range: one aggregated check, the first offender captured.
    size_t out_of_range = 0, first_frame = 0, first_index = 0;
    double first_value = 0.0;
    for (size_t k = 0; k < v.size(); ++k)
        for (size_t i = 0; i < v[k].data.size(); ++i) {
            const double s = v[k].data[i];
            if (!(s >= 0.0 && s <= 1.0) && out_of_range++ == 0) {
                first_frame = k;
                first_index = i;
                first_value = s;
            }
        }
    CAPTURE(out_of_range, first_frame, first_index, first_value);
    REQUIRE(out_of_range == 0);
}

TEST_CASE("orbit_flat_model_frames_identical", "[recon][orbit]") {
    // Equal depth everywhere: every pixel's shift is tan(theta) * 0, so every frame is the base.
    for (const double zc : {0.0, 0.37, 1.0}) {
        const CavityModel m = plain_model(40, 30, {17.0, 11.0}, [zc](int, int) { return zc; });
        const Image base = render_cavity_2d(m, ReconConfig{});
        const std::vector<Image> v = orbit_views(m, ReconConfig{}, ExportConfig{}, OrbitConfig{});
        REQUIRE(v.size() == 36);
        for (const Image& f : v) CHECK(f.data == base.data);
    }
}

TEST_CASE("orbit_splat_zbuffer_and_holes", "[recon][orbit]") {
    SECTION("apex depth 0: forward shifts, nearer wins, interior hole, empty row, left end") {
        // Row 0: z = 2 at x = 2 -> lands on 4, where the z = 0 pixel 4 also lands: pixel 4 wins
        //        (smaller z), target 2 is a hole between 1 and 3.
        // Row 1: z = 9 everywhere -> every pixel leaves the 8-px row: the row is 0.
        // Row 2: z = 3 everywhere -> sources 0..4 land on 3..7; 0..2 take the nearest (source 0).
        const CavityModel m = plain_model(8, 3, {0.0, 0.0}, [](int x, int y) {
            if (y == 0) return x == 2 ? 2.0 : 0.0;
            return y == 1 ? 9.0 : 3.0;
        });
        const UnitShift u = unit_shift(m);
        const Image base = render_cavity_2d(m, ReconConfig{});
        const std::vector<Image> v = orbit_views(m, ReconConfig{}, u.ecfg, u.ocfg);
        REQUIRE(v.size() == 4);
        const Image& f = v[1];  // theta = +10 deg: shift = z - z_apex
        for (int x : {0, 1, 3, 5, 6, 7}) check_pixel(f, x, 0, base, x, 0);
        check_pixel(f, 4, 0, base, 4, 0);  // z-buffer: z 0 beats z 2
        check_lerp(f, 2, 0, base, 1, 3, 0.5);
        for (int x = 0; x < 8; ++x)
            for (int c = 0; c < 3; ++c) CHECK(f.at(x, 1, c) == 0.0);
        for (int x = 0; x < 3; ++x) check_pixel(f, x, 2, base, 0, 2);
        for (int x = 3; x < 8; ++x) check_pixel(f, x, 2, base, x - 3, 2);

        // Frame 3 (theta = -10 deg) mirrors it: row 2 lands on -3..4, the right end takes source 7.
        const Image& g = v[3];
        for (int x = 0; x < 5; ++x) check_pixel(g, x, 2, base, x + 3, 2);
        for (int x = 5; x < 8; ++x) check_pixel(g, x, 2, base, 7, 2);
        check_pixel(g, 0, 0, base, 0, 0);  // pixel 2 (z 2) lands on 0 too: pixel 0 (z 0) wins
        check_lerp(g, 2, 0, base, 1, 3, 0.5);
    }
    SECTION("apex depth 2: the nearer pixel moves and wins over a farther one") {
        // z_apex = z(0, 1) = 2. Row 0: z = 0 at x = 3 shifts by -2 onto 1, where the z = 2 pixel 1
        // stays: pixel 3 wins; target 3 is a hole between 2 and 4. Row 1 (the apex row) is fixed.
        const CavityModel m = plain_model(8, 2, {0.4, 0.6}, [](int x, int y) { return y == 0 && x == 3 ? 0.0 : 2.0; });
        const UnitShift u = unit_shift(m);
        const Image base = render_cavity_2d(m, ReconConfig{});
        const Image f = orbit_views(m, ReconConfig{}, u.ecfg, u.ocfg)[1];
        check_pixel(f, 1, 0, base, 3, 0);
        check_lerp(f, 3, 0, base, 2, 4, 0.5);
        for (int x : {0, 2, 4, 5, 6, 7}) check_pixel(f, x, 0, base, x, 0);
        for (int x = 0; x < 8; ++x) check_pixel(f, x, 1, base, x, 1);
    }
    SECTION("a wide hole interpolates linearly") {
        // Row 0: x = 1..3 at z 4 move onto 5..7, where the z = 0 pixels 5..7 stay and win; 0 and 4
        // stay; targets 1..3 are a 3-px hole between 0 and 4.
        const CavityModel m = plain_model(8, 1, {0.0, 0.0}, [](int x, int) { return x >= 1 && x <= 3 ? 4.0 : 0.0; });
        const UnitShift u = unit_shift(m);
        const Image base = render_cavity_2d(m, ReconConfig{});
        const Image f = orbit_views(m, ReconConfig{}, u.ecfg, u.ocfg)[1];
        for (int x : {0, 4, 5, 6, 7}) check_pixel(f, x, 0, base, x, 0);
        for (int x = 1; x < 4; ++x) check_lerp(f, x, 0, base, 0, 4, x / 4.0);
    }
}

TEST_CASE("orbit_gif_has_n_frames_and_width", "[recon][orbit]") {
    TempDir tmp("gdg_orbit");
    const CavityModel m = blister_model();
    SECTION("blister, defaults: 36 frames at the model's own 128 px") {
        const std::string path = (tmp.path / "orbit.gif").string();
        write_orbit_gif(path, m, ReconConfig{}, ExportConfig{}, OrbitConfig{});
        CHECK(no_temps(path));
        const GifFile g = parse_gif(gdg::test::read_file_bytes(path));
        REQUIRE(g.frames.size() == 36);
        CHECK(g.width <= 760);
        CHECK(g.width == 128);
        CHECK(g.height == 96);
        CHECK(g.loops == 0);
        for (const auto& f : g.frames) {
            CHECK(f.delay_cs == 8);
            CHECK(f.width == 128);
        }
        // Every frame is the frame-0 STF of its view (no per-frame stretch), up to palette error.
        const std::vector<Image> views = orbit_views(m, ReconConfig{}, ExportConfig{}, OrbitConfig{});
        const gdg::StfParams p = gdg::stf_params(views[0]);
        for (size_t k = 0; k < views.size(); k += 9) {
            const std::vector<std::uint8_t> want = gdg::apply_stf_u8(views[k], p);
            double err = 0.0;
            for (size_t i = 0; i < want.size(); ++i) err += std::abs(int(g.frames[k].rgb[i]) - int(want[i]));
            CHECK(err / static_cast<double>(want.size()) <= 4.0);
        }
    }
    SECTION("max_width downscales every frame, aspect kept") {
        OrbitConfig o;
        o.max_width = 64;
        o.frames = 6;
        o.delay_cs = 3;
        const std::string path = (tmp.path / "small.gif").string();
        write_orbit_gif(path, m, ReconConfig{}, ExportConfig{}, o);
        const GifFile g = parse_gif(gdg::test::read_file_bytes(path));
        REQUIRE(g.frames.size() == 6);
        CHECK(g.width == 64);
        CHECK(g.height == 48);
        for (const auto& f : g.frames) CHECK(f.delay_cs == 3);
    }
    SECTION("a wide model is held to 760 px") {
        const CavityModel wide = plain_model(1600, 120, {800.0, 60.0}, [](int x, int y) {
            const double r = std::hypot(x - 800.0, (y - 60.0) * 8.0) / 800.0;
            return std::min(1.0, r * r);
        });
        const std::string path = (tmp.path / "wide.gif").string();
        write_orbit_gif(path, wide, ReconConfig{}, ExportConfig{}, OrbitConfig{});
        const GifFile g = parse_gif(gdg::test::read_file_bytes(path));
        CHECK(g.frames.size() == 36);
        CHECK(g.width == 760);
        CHECK(g.height == 57);  // round(120 * 760 / 1600) = 57
    }
}

TEST_CASE("orbit_errors", "[recon][orbit]") {
    TempDir tmp("gdg_orbit");
    const CavityModel m = plain_model(12, 9, {5.0, 4.0}, [](int x, int y) { return 0.01 * (x + y); });
    const std::string path = (tmp.path / "o.gif").string();
    auto refused = [&](const CavityModel& mm, const ExportConfig& e, const OrbitConfig& o) {
        CHECK_THROWS_AS(orbit_views(mm, ReconConfig{}, e, o), std::invalid_argument);
        CHECK_THROWS_AS(write_orbit_gif(path, mm, ReconConfig{}, e, o), std::invalid_argument);
        CHECK_FALSE(fs::exists(path));
    };
    const ExportConfig e;
    auto with = [](auto f) {
        OrbitConfig o;
        f(o);
        return o;
    };
    refused(m, e, with([](OrbitConfig& o) { o.frames = 0; }));
    refused(m, e, with([](OrbitConfig& o) { o.amplitude_deg = 90.0; }));
    refused(m, e, with([](OrbitConfig& o) { o.amplitude_deg = -95.0; }));
    refused(m, e, with([](OrbitConfig& o) { o.amplitude_deg = std::numeric_limits<double>::quiet_NaN(); }));
    refused(m, e, with([](OrbitConfig& o) { o.max_width = 0; }));
    refused(m, e, with([](OrbitConfig& o) { o.delay_cs = -1; }));
    refused(m, e, with([](OrbitConfig& o) { o.delay_cs = 65536; }));
    ExportConfig neg;
    neg.depth_ratio = -0.5;
    refused(m, neg, OrbitConfig{});
    ExportConfig inf;
    inf.depth_ratio = std::numeric_limits<double>::infinity();
    refused(m, inf, OrbitConfig{});
    CavityModel nan_z = m;
    nan_z.z.at(3, 2) = std::numeric_limits<double>::quiet_NaN();
    refused(nan_z, e, OrbitConfig{});
    CHECK(thrown_message([&] { orbit_views(nan_z, ReconConfig{}, e, OrbitConfig{}); }).find("(3, 2)") !=
          std::string::npos);
    CavityModel nan_e = m;
    nan_e.emissivity.at(7, 1, 2) = std::numeric_limits<double>::infinity();
    refused(nan_e, e, OrbitConfig{});
    CHECK(thrown_message([&] { orbit_views(nan_e, ReconConfig{}, e, OrbitConfig{}); }).find("(7, 1)") !=
          std::string::npos);
    CavityModel nan_dust = m;
    nan_dust.dust.at(0, 8) = std::numeric_limits<double>::quiet_NaN();
    refused(nan_dust, e, OrbitConfig{});
    CavityModel mono_e = m;
    mono_e.emissivity = Image(12, 9, 1, 0.5);
    refused(mono_e, e, OrbitConfig{});
    CavityModel nan_apex = m;
    nan_apex.apex_xy.second = std::numeric_limits<double>::infinity();
    refused(nan_apex, e, OrbitConfig{});
    refused(plain_model(0, 0, {0.0, 0.0}, [](int, int) { return 0.0; }), e, OrbitConfig{});

    // The apex may lie up to its own size outside the grid (-nx <= x <= 2 nx, -ny <= y <= 2 ny).
    // At the bound the frames still render real content: frame 0 is the base, the others move.
    for (const std::pair<double, double> apex : {std::pair<double, double>{-12.0, 18.0}, {24.0, -9.0}}) {
        INFO("apex (" << apex.first << ", " << apex.second << ")");
        CavityModel edge = m;
        edge.apex_xy = apex;
        const Image base = render_cavity_2d(edge, ReconConfig{});
        ExportConfig deep;
        deep.depth_ratio = 3.0;  // a shift of several pixels on this small model
        const std::vector<Image> views = orbit_views(edge, ReconConfig{}, deep, OrbitConfig{});
        REQUIRE(views.size() == 36);
        CHECK(views[0].data == base.data);
        double max0 = 0.0;
        for (double v : views[0].data) max0 = std::max(max0, v);
        CHECK(max0 > 0.0);
        for (const Image& v : views) {
            double mx = 0.0;
            for (double d : v.data) mx = std::max(mx, d);
            CHECK(mx > 0.0);  // no frame is black
        }
        CHECK(views[9].data != views[0].data);
        CHECK(mean_abs_diff(views[9], views[0]) > 0.0);
    }
    // Beyond it the apex is refused (the radius D would swallow every shift), with the size named.
    for (const std::pair<double, double> apex :
         {std::pair<double, double>{-12.5, 4.0}, {4.0, 18.5}, {24.5, 4.0}, {4.0, -9.5}, {1e300, -1e300}}) {
        CavityModel far = m;
        far.apex_xy = apex;
        refused(far, e, OrbitConfig{});
    }
    CavityModel far = m;
    far.apex_xy = {-12.5, 4.0};
    CHECK(thrown_message([&] { orbit_views(far, ReconConfig{}, e, OrbitConfig{}); }) ==
          "apex (-12.5, 4) is outside the model grid (12x9) by more than its size");
    CHECK(thrown_message([&] { write_orbit_gif(path, far, ReconConfig{}, e, OrbitConfig{}); }) ==
          "apex (-12.5, 4) is outside the model grid (12x9) by more than its size");
    // Negative amplitudes are a mirrored orbit.
    CHECK(orbit_views(m, ReconConfig{}, e, with([](OrbitConfig& o) { o.amplitude_deg = -30.0; })).size() == 36);

    // An unwritable destination fails loudly, naming the path, with no partial file.
    const fs::path locked = tmp.path / "locked";
    fs::create_directory(locked);
    REQUIRE(chmod(locked.c_str(), 0500) == 0);
    const std::string bad = (locked / "o.gif").string();
    CHECK(thrown_message([&] { write_orbit_gif(bad, m, ReconConfig{}, e, OrbitConfig{}); }) ==
          "cannot write " + bad + ": " + std::strerror(EACCES));
    CHECK_FALSE(fs::exists(bad));
    CHECK(no_temps(bad));
}

TEST_CASE("orbit_concurrent_calls_identical", "[recon][orbit]") {
    const CavityModel m = blister_model();
    std::vector<Image> a, b;
    std::thread t1([&] { a = orbit_views(m, ReconConfig{}, ExportConfig{}, OrbitConfig{}); });
    std::thread t2([&] { b = orbit_views(m, ReconConfig{}, ExportConfig{}, OrbitConfig{}); });
    t1.join();
    t2.join();
    REQUIRE(a.size() == 36);
    REQUIRE(b.size() == 36);
    for (size_t k = 0; k < a.size(); ++k) CHECK(a[k].data == b[k].data);

    TempDir tmp("gdg_orbit");
    const std::string pa = (tmp.path / "a.gif").string(), pb = (tmp.path / "b.gif").string();
    std::thread w1([&] { write_orbit_gif(pa, m, ReconConfig{}, ExportConfig{}, OrbitConfig{}); });
    std::thread w2([&] { write_orbit_gif(pb, m, ReconConfig{}, ExportConfig{}, OrbitConfig{}); });
    w1.join();
    w2.join();
    const auto ba = gdg::test::read_file_bytes(pa);
    CHECK(ba == gdg::test::read_file_bytes(pb));
    CHECK(parse_gif(ba).frames.size() == 36);
}

TEST_CASE("orbit_rejects_mismatched_plane_shapes", "[recon][orbit]") {
    // Planes replaced after construction are refused before any pixel is read, by both entry points.
    TempDir tmp("gdg_orbit");
    const std::string path = (tmp.path / "o.gif").string();
    for (const Image& bad : {Image(11, 9), Image(13, 9), Image(12, 8)}) {
        CavityModel m = plain_model(12, 9, {5.0, 4.0}, [](int x, int y) { return 0.01 * (x + y); });
        m.dust = bad;
        CHECK_THROWS_WITH(orbit_views(m, ReconConfig{}, ExportConfig{}, OrbitConfig{}),
                          Catch::Matchers::ContainsSubstring("shape mismatch"));
        CHECK_THROWS_WITH(write_orbit_gif(path, m, ReconConfig{}, ExportConfig{}, OrbitConfig{}),
                          Catch::Matchers::ContainsSubstring("shape mismatch"));
        CavityModel e = plain_model(12, 9, {5.0, 4.0}, [](int x, int y) { return 0.01 * (x + y); });
        e.emissivity = Image(bad.width, bad.height, 3);
        CHECK_THROWS_WITH(orbit_views(e, ReconConfig{}, ExportConfig{}, OrbitConfig{}),
                          Catch::Matchers::ContainsSubstring("shape mismatch"));
        CHECK_FALSE(fs::exists(path));
    }
}
