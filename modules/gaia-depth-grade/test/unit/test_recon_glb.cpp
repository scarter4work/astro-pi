// Task 23 (Phase G2): the native GLB export of the cavity model. Analytic checks on small
// hand-built models and on the C++ synthetic blister (test/util/Blister.h); the file is decoded
// by the test-only reader test/util/GlbRead.h.
#include <catch_amalgamated.hpp>

#include "Blister.h"
#include "GlbRead.h"
#include "core/Format.h"
#include "core/recon/Glb.h"
#include "core/recon/Reconstruct.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace gdg::recon;
using gdg::Image;
using gdg::test::GlbFile;
using gdg::test::Json;
using gdg::test::parse_glb;
using gdg::test::read_accessor;

namespace {

namespace fs = std::filesystem;

/// An nx x ny model: z from `zf`, emissivity with out-of-range values (clipping is visible),
/// dust from `df`, apex (4, 3).
CavityModel grid_model(int nx, int ny, std::optional<double> pc, const std::function<double(int, int)>& zf,
                       const std::function<double(int, int)>& df) {
    Image z(nx, ny), e(nx, ny, 3), d(nx, ny);
    for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x) {
            z.at(x, y) = zf(x, y);
            e.at(x, y, 0) = 0.1 * x - 0.2;   // negative on the left: clipped to 0
            e.at(x, y, 1) = 0.25 * y;        // above 1 at the bottom: clipped to 1
            e.at(x, y, 2) = 0.5;
            d.at(x, y) = df(x, y);
        }
    CavityMeta meta;
    meta.capture_mode = "sho";
    meta.ionizer_source = "override";
    meta.footprint_px = 42;
    return CavityModel(std::move(z), std::move(e), std::move(d), pc, {4.0, 3.0}, meta);
}

double ramp_z(int x, int y) { return x == 0 ? 0.0 : 0.05 * x + 0.03 * y; }
double no_dust(int, int) { return 0.0; }

CavityModel model_9x7(std::optional<double> pc = std::nullopt) { return grid_model(9, 7, pc, ramp_z, no_dust); }

CavityModel blister_model() {
    const gdg::test::Blister b = gdg::test::make_blister();
    ReconInputs in;
    in.mode = "sho";
    in.channels = {{"ha", b.ha}, {"oiii", b.oiii}, {"sii", b.sii}};
    in.ionizer_override = ionizer_from_xy(b.truth.apex_x, b.truth.apex_y, std::nullopt);
    return reconstruct(in, ReconConfig{}, gdg::Log{}).model;
}

ExportConfig with_decimate(int d) {
    ExportConfig c;
    c.decimate = d;
    return c;
}

const Json& at(const Json& j, size_t i) { return j[i]; }

/// The depth scale D the contract defines: depth_ratio * (largest apex distance with z > 0).
double depth_scale(const CavityModel& m, double depth_ratio) {
    double r = 0.0;
    bool any = false;
    for (int y = 0; y < m.z.height; ++y)
        for (int x = 0; x < m.z.width; ++x)
            if (m.z.at(x, y) > 0.0) {
                any = true;
                r = std::max(r, std::hypot(x - m.apex_xy.first, y - m.apex_xy.second));
            }
    if (!any)
        for (int y = 0; y < m.z.height; ++y)
            for (int x = 0; x < m.z.width; ++x)
                r = std::max(r, std::hypot(x - m.apex_xy.first, y - m.apex_xy.second));
    return depth_ratio * r;
}

/// A fresh private directory for one test.
struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "gdg_glb_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(path, ec))
            fs::permissions(e.path(), fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
};

/// True when the directory of `path` holds no `<name>.tmp*` entry (the atomic writer's temporaries).
bool no_temps(const std::string& path) {
    const fs::path p(path);
    const std::string prefix = p.filename().string() + ".tmp";
    std::error_code ec;
    if (!fs::exists(p.parent_path(), ec)) return true;
    for (const auto& e : fs::directory_iterator(p.parent_path()))
        if (e.path().filename().string().rfind(prefix, 0) == 0) return false;
    return true;
}

std::string thrown_message(const std::function<void()>& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace

TEST_CASE("glb_header_and_chunks_valid", "[recon][glb]") {
    const CavityModel m = blister_model();
    const std::vector<std::uint8_t> bytes = build_glb(m, ExportConfig{});
    const GlbFile g = parse_glb(bytes);
    CHECK(g.magic == 0x46546C67);  // "glTF"
    CHECK(std::memcmp(bytes.data(), "glTF", 4) == 0);
    CHECK(g.version == 2);
    CHECK(g.length == bytes.size());
    CHECK(g.json_chunk_type == 0x4E4F534A);  // "JSON"
    CHECK(g.bin_chunk_type == 0x004E4942);   // "BIN\0"
    CHECK(g.json_chunk_length % 4 == 0);
    CHECK(g.bin_chunk_length % 4 == 0);
    CHECK(12 + 8 + g.json_chunk_length + 8 + g.bin_chunk_length == bytes.size());
    // JSON padding is spaces, and the buffer's byteLength fits the BIN chunk.
    const size_t last = g.json_text.find_last_not_of(' ');
    CHECK(g.json_text[last] == '}');
    CHECK(g.json["buffers"].arr.size() == 1);
    CHECK_FALSE(at(g.json["buffers"], 0).has("uri"));
    CHECK(at(g.json["buffers"], 0)["byteLength"].num <= g.bin_chunk_length);
    CHECK(g.bin_chunk_length - at(g.json["buffers"], 0)["byteLength"].num < 4);
    // Every bufferView starts on a 4-byte boundary.
    for (const Json& v : g.json["bufferViews"].arr) CHECK(static_cast<long long>(v["byteOffset"].num) % 4 == 0);

    // The size function is exact given the JSON length, and an upper bound with the reserve.
    const bool has_dust = g.json["bufferViews"].arr.size() == 5;
    CHECK(glb_size_bytes(m.z.width, m.z.height, 2, has_dust, g.json_chunk_length) == bytes.size());
    CHECK(glb_size_bytes(m.z.width, m.z.height, 2, has_dust) >= bytes.size());
    CHECK(g.json_chunk_length <= GLB_JSON_RESERVE);
}

TEST_CASE("glb_vertex_grid_count", "[recon][glb]") {
    const CavityModel m = model_9x7();
    {
        const GlbFile g = parse_glb(build_glb(m, with_decimate(2)));
        CHECK(at(g.json["accessors"], 0)["count"].num == 20);  // 5 x 4
        CHECK(at(g.json["accessors"], 1)["count"].num == 20);
        CHECK(at(g.json["accessors"], 2)["count"].num == 72);  // 4 x 3 cells x 6
        const std::vector<std::uint32_t> idx = read_accessor<std::uint32_t>(g, 2);
        REQUIRE(idx.size() == 72);
        for (std::uint32_t i : idx) CHECK(i < 20);
        // Vertex index = row * cols + col: vertex 6 is (col 1, row 1) = pixel (2, 2).
        const std::vector<float> p = read_accessor<float>(g, 0);
        CHECK(p[3 * 6 + 0] == static_cast<float>(2.0 - 4.0));
        CHECK(p[3 * 6 + 1] == static_cast<float>(-(2.0 - 3.0)));
    }
    {
        const GlbFile g = parse_glb(build_glb(m, with_decimate(1)));
        CHECK(at(g.json["accessors"], 0)["count"].num == 63);
        CHECK(at(g.json["accessors"], 2)["count"].num == 8 * 6 * 6);
    }
    CHECK(glb_grid_count(9, 2) == 5);
    CHECK(glb_grid_count(7, 2) == 4);
    // Under 2 x 2 vertices there is no triangle to write; a bad step is refused.
    CHECK_THROWS_AS(build_glb(m, with_decimate(7)), std::invalid_argument);
    CHECK_THROWS_AS(build_glb(m, with_decimate(0)), std::invalid_argument);
}

TEST_CASE("glb_surface_is_concave", "[recon][glb]") {
    const gdg::test::Blister b = gdg::test::make_blister();
    const CavityModel m = blister_model();
    const GlbFile g = parse_glb(build_glb(m, ExportConfig{}));
    const std::vector<float> p = read_accessor<float>(g, 0);
    const int d = 2, cols = glb_grid_count(m.z.width, d), rows = glb_grid_count(m.z.height, d);
    REQUIRE(p.size() == static_cast<size_t>(3 * cols * rows));

    double best = std::numeric_limits<double>::infinity(), apex_z = 0.0, rim_sum = 0.0;
    long rim_n = 0;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const size_t v = static_cast<size_t>(r * cols + c);
            const double dist = std::hypot(c * d - b.truth.apex_x, r * d - b.truth.apex_y);
            if (dist < best) {
                best = dist;
                apex_z = p[3 * v + 2];
            }
            const double rho = dist / b.truth.radius_px;
            if (rho >= 0.9 && rho <= 1.0) {
                rim_sum += p[3 * v + 2];
                ++rim_n;
            }
        }
    REQUIRE(rim_n > 0);
    CHECK(apex_z < rim_sum / static_cast<double>(rim_n));
    CHECK(apex_z < 0.0);  // the apex lies behind the sky plane, away from the viewer at +Z
}

TEST_CASE("glb_position_min_max_match_data", "[recon][glb]") {
    CavityModel m = blister_model();
    m.dust.at(10, 10) = 0.5;  // force the dust mesh: both position accessors are checked
    const GlbFile g = parse_glb(build_glb(m, ExportConfig{}));
    REQUIRE(g.json["accessors"].arr.size() == 5);
    for (size_t a : {size_t{0}, size_t{3}}) {
        const Json& acc = at(g.json["accessors"], a);
        CHECK(acc["type"].str == "VEC3");
        const std::vector<float> p = read_accessor<float>(g, a);
        for (size_t k = 0; k < 3; ++k) {
            float lo = std::numeric_limits<float>::infinity(), hi = -lo;
            for (size_t i = k; i < p.size(); i += 3) {
                lo = std::min(lo, p[i]);
                hi = std::max(hi, p[i]);
            }
            CHECK(static_cast<float>(at(acc["min"], k).num) == lo);
            CHECK(static_cast<float>(at(acc["max"], k).num) == hi);
            // The JSON value is the float itself, not a nearby double.
            CHECK(at(acc["min"], k).num == static_cast<double>(lo));
            CHECK(at(acc["max"], k).num == static_cast<double>(hi));
        }
    }
}

TEST_CASE("glb_front_faces_toward_viewer", "[recon][glb]") {
    const CavityModel m = grid_model(9, 7, std::nullopt, [](int, int) { return 0.0; }, no_dust);
    const GlbFile g = parse_glb(build_glb(m, with_decimate(1)));
    const std::vector<float> p = read_accessor<float>(g, 0);
    const std::vector<std::uint32_t> idx = read_accessor<std::uint32_t>(g, 2);
    REQUIRE(idx.size() % 3 == 0);
    auto normal_z = [&](size_t t) {
        const float* a = &p[3 * idx[3 * t]];
        const float* b = &p[3 * idx[3 * t + 1]];
        const float* c = &p[3 * idx[3 * t + 2]];
        const double ux = b[0] - a[0], uy = b[1] - a[1], vx = c[0] - a[0], vy = c[1] - a[1];
        return ux * vy - uy * vx;  // z of (b - a) x (c - a)
    };
    CHECK(normal_z(0) > 0.0);
    for (size_t t = 0; t < idx.size() / 3; ++t) CHECK(normal_z(t) > 0.0);
    for (size_t i = 2; i < p.size(); i += 3) CHECK(p[i] == 0.0f);
}

TEST_CASE("glb_units_pc_when_scale_known", "[recon][glb]") {
    const double u = 0.0625;
    const CavityModel m = model_9x7(u);
    const ExportConfig cfg = with_decimate(2);
    const GlbFile g = parse_glb(build_glb(m, cfg));
    const Json& extras = g.json["asset"]["extras"];
    CHECK(extras["units"].str == "pc");
    CHECK(extras["depth_ratio"].num == cfg.depth_ratio);
    const std::vector<float> p = read_accessor<float>(g, 0);
    const double D = depth_scale(m, cfg.depth_ratio);
    const int cols = 5;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < cols; ++c) {
            const size_t v = static_cast<size_t>(r * cols + c);
            const int x = 2 * c, y = 2 * r;
            CHECK(p[3 * v + 0] == static_cast<float>((x - 4.0) * u));
            CHECK(p[3 * v + 1] == static_cast<float>(-(y - 3.0) * u));
            CHECK(p[3 * v + 2] == static_cast<float>(-(m.z.at(x, y) * D * u)));
        }
}

TEST_CASE("glb_units_px_without_scale", "[recon][glb]") {
    const CavityModel m = model_9x7(std::nullopt);
    const ExportConfig cfg = with_decimate(2);
    const GlbFile g = parse_glb(build_glb(m, cfg));
    CHECK(g.json["asset"]["extras"]["units"].str == "px");
    const std::vector<float> p = read_accessor<float>(g, 0);
    const double D = depth_scale(m, cfg.depth_ratio);
    CHECK(D > 0.0);
    CHECK(p[0] == -4.0f);  // vertex 0 = pixel (0, 0), apex (4, 3), u = 1
    CHECK(p[1] == 3.0f);
    CHECK(p[3 * 1 + 0] - p[0] == 2.0f);  // columns 2 px apart
    CHECK(p[3 * 19 + 2] == static_cast<float>(-(m.z.at(8, 6) * D)));

    // A pc_per_px that is not a positive finite scale is refused, never exported as px.
    CavityModel bad = m;
    bad.pc_per_px = 0.0;
    CHECK_THROWS_AS(build_glb(bad, cfg), std::invalid_argument);
}

TEST_CASE("glb_dust_mesh_only_when_dust", "[recon][glb]") {
    const ExportConfig cfg = with_decimate(2);
    {
        // max(dust) below dust_min: no dust mesh at all.
        const CavityModel m = grid_model(9, 7, std::nullopt, ramp_z, [](int, int) { return 0.009; });
        const GlbFile g = parse_glb(build_glb(m, cfg));
        CHECK(g.json["bufferViews"].arr.size() == 3);
        CHECK(g.json["accessors"].arr.size() == 3);
        CHECK(g.json["materials"].arr.size() == 1);
        CHECK(g.json["meshes"].arr.size() == 1);
    }
    {
        // max(dust) == dust_min, on a pixel the grid does not sample: the dust mesh is written.
        const CavityModel m = grid_model(9, 7, std::nullopt, ramp_z,
                                         [](int x, int y) { return x == 3 && y == 3 ? 0.01 : 0.0; });
        const GlbFile g = parse_glb(build_glb(m, cfg));
        CHECK(g.json["bufferViews"].arr.size() == 5);
        CHECK(g.json["accessors"].arr.size() == 5);
        CHECK(g.json["materials"].arr.size() == 2);
    }
    const CavityModel m =
        grid_model(9, 7, 2.0, ramp_z, [](int x, int y) { return x >= 6 ? 0.4 + 0.1 * y : 0.0; });
    const GlbFile g = parse_glb(build_glb(m, cfg));
    REQUIRE(g.json["bufferViews"].arr.size() == 5);
    REQUIRE(g.json["accessors"].arr.size() == 5);
    REQUIRE(g.json["materials"].arr.size() == 2);
    const Json& mat1 = at(g.json["materials"], 1);
    CHECK(mat1["alphaMode"].str == "BLEND");
    CHECK(mat1["extensions"].has("KHR_materials_unlit"));

    // The dust primitive: dust positions + colours, the surface's index accessor, material 1.
    bool found = false;
    for (const Json& mesh : g.json["meshes"].arr)
        for (const Json& prim : mesh["primitives"].arr)
            if (prim["material"].num == 1) {
                found = true;
                CHECK(prim["attributes"]["POSITION"].num == 3);
                CHECK(prim["attributes"]["COLOR_0"].num == 4);
                CHECK(prim["indices"].num == 2);
            }
    CHECK(found);

    const double u = 2.0, D = depth_scale(m, cfg.depth_ratio);
    const std::vector<float> sp = read_accessor<float>(g, 0);
    const std::vector<float> dp = read_accessor<float>(g, 3);
    const std::vector<float> dc = read_accessor<float>(g, 4);
    CHECK(at(g.json["accessors"], 4)["type"].str == "VEC4");
    REQUIRE(dp.size() == sp.size());
    REQUIRE(dc.size() == 4 * 20);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 5; ++c) {
            const size_t v = static_cast<size_t>(r * 5 + c);
            const int x = 2 * c, y = 2 * r;
            CHECK(dp[3 * v] == sp[3 * v]);
            CHECK(dp[3 * v + 1] == sp[3 * v + 1]);
            CHECK(dp[3 * v + 2] == static_cast<float>(-(m.z.at(x, y) * D * u) + cfg.dust_offset_frac * D * u));
            CHECK(dp[3 * v + 2] > sp[3 * v + 2]);  // nearer the viewer
            CHECK(dc[4 * v + 0] == 0.05f);
            CHECK(dc[4 * v + 1] == 0.04f);
            CHECK(dc[4 * v + 2] == 0.03f);
            CHECK(dc[4 * v + 3] == static_cast<float>(m.dust.at(x, y)));
        }

    // Surface colours are clip(emissivity, 0, 1).
    const std::vector<float> sc = read_accessor<float>(g, 1);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 5; ++c) {
            const size_t v = static_cast<size_t>(r * 5 + c);
            for (int k = 0; k < 3; ++k)
                CHECK(sc[3 * v + k] == static_cast<float>(clip(m.emissivity.at(2 * c, 2 * r, k), 0.0, 1.0)));
        }

    {
        // Dust outside [0, 1] is clipped in the alpha channel: -0.5 -> 0, 1.7 -> 1.
        const CavityModel o = grid_model(9, 7, std::nullopt, ramp_z,
                                         [](int x, int) { return x < 4 ? -0.5 : (x > 4 ? 1.7 : 0.25); });
        const GlbFile go = parse_glb(build_glb(o, cfg));
        REQUIRE(go.json["accessors"].arr.size() == 5);
        const std::vector<float> oc = read_accessor<float>(go, 4);
        REQUIRE(oc.size() == 4 * 20);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 5; ++c) {
                const float expect = c < 2 ? 0.0f : (c > 2 ? 1.0f : 0.25f);  // x = 2c vs 4
                CHECK(oc[4 * static_cast<size_t>(r * 5 + c) + 3] == expect);
            }
    }
}

TEST_CASE("glb_honesty_and_meta_in_extras", "[recon][glb]") {
    const CavityModel m = blister_model();
    const GlbFile g = parse_glb(build_glb(m, ExportConfig{}));
    const Json& asset = g.json["asset"];
    CHECK(asset["version"].str == "2.0");
    CHECK(asset["generator"].str == "GaiaDepthGrade NebulaReconstruct");
    CHECK(asset["extras"]["honesty"].str == "Reconstructed HII cavity; physically motivated, not measured gas depth");
    CHECK(asset["extras"]["honesty"].str == RECON_HONESTY);
    CHECK(asset["extras"]["depth_ratio"].num == 0.5);

    // meta is the cavity_meta_json object, embedded as JSON (not a string).
    const Json& meta = asset["extras"]["meta"];
    const Json expect = Json::parse(cavity_meta_json(m.meta));
    REQUIRE(meta.kind == Json::Object);
    CHECK(meta.obj.size() == expect.obj.size());
    CHECK(meta["capture_mode"].str == "sho");
    CHECK(meta["ionizer_source"].str == m.meta.ionizer_source);
    CHECK(meta["footprint_px"].num == static_cast<double>(m.meta.footprint_px));
    CHECK(meta["ratio_confidence"].num == m.meta.ratio_confidence);
    CHECK(meta["w_ionization_effective"].num == m.meta.w_ionization_effective);
    CHECK(meta["is_linear"].b == m.meta.is_linear);
    // The raw text carries the object verbatim.
    CHECK(g.json_text.find("\"meta\":" + cavity_meta_json(m.meta)) != std::string::npos);

    // Unlit, double-sided material 0, with the extension declared.
    bool unlit_used = false;
    for (const Json& e : g.json["extensionsUsed"].arr) unlit_used |= e.str == "KHR_materials_unlit";
    CHECK(unlit_used);
    const Json& mat0 = at(g.json["materials"], 0);
    CHECK(mat0["extensions"].has("KHR_materials_unlit"));
    CHECK(mat0["doubleSided"].b);

    // Strings are escaped: quotes and control characters in meta strings stay valid JSON.
    CavityModel odd = m;
    odd.meta.ionizer_source = "a \"quoted\"\\ name\n\t\x01";
    const GlbFile g2 = parse_glb(build_glb(odd, ExportConfig{}));
    CHECK(g2.json["asset"]["extras"]["meta"]["ionizer_source"].str == odd.meta.ionizer_source);
    CHECK(std::none_of(g2.json_text.begin(), g2.json_text.end(),
                       [](char ch) { return static_cast<unsigned char>(ch) < 0x20; }));  // no raw control bytes
    // Exact escaped text: named escapes, \u00xx (lowercase) for other control bytes; DEL and UTF-8 as is.
    CHECK(gdg::json_quote("a\"\\\b\f\n\r\t\x01\x1f\x7f\xc3\xa9") ==
          "\"a\\\"\\\\\\b\\f\\n\\r\\t\\u0001\\u001f\x7f\xc3\xa9\"");
    CHECK(gdg::json_quote("") == "\"\"");
    CavityMeta cm;
    cm.ionizer_source = "x\x01\x1b";
    CHECK(cavity_meta_json(cm).find(", \"ionizer_source\": \"x\\u0001\\u001b\", ") != std::string::npos);
    CHECK(g2.json_text.find("\\n\\t\\u0001\"") != std::string::npos);

    // A non-finite meta value would make invalid JSON: refused.
    CavityModel nan_meta = m;
    nan_meta.meta.ratio_confidence = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(build_glb(nan_meta, ExportConfig{}), std::invalid_argument);
}

TEST_CASE("glb_write_atomic_and_errors", "[recon][glb]") {
    const CavityModel m = model_9x7();
    const ExportConfig cfg = with_decimate(1);
    const std::vector<std::uint8_t> expect = build_glb(m, cfg);
    TempDir tmp;

    SECTION("writes the bytes and leaves no temporary") {
        const std::string path = (tmp.path / "cavity.glb").string();
        write_glb(path, m, cfg);
        CHECK(gdg::test::read_file_bytes(path) == expect);
        CHECK(no_temps(path));
    }
    SECTION("an existing file is replaced") {
        const std::string path = (tmp.path / "cavity.glb").string();
        {
            std::ofstream(path) << "an older, longer file that must not survive in any part of the output";
        }
        write_glb(path, m, cfg);
        CHECK(gdg::test::read_file_bytes(path) == expect);
        CHECK(no_temps(path));
    }
    SECTION("unwritable directory") {
        const fs::path dir = tmp.path / "locked";
        fs::create_directory(dir);
        REQUIRE(chmod(dir.c_str(), 0500) == 0);
        const std::string path = (dir / "cavity.glb").string();
        const std::string msg = thrown_message([&] { write_glb(path, m, cfg); });
        CHECK(msg == "cannot write " + path + ": " + std::strerror(EACCES));
        CHECK_FALSE(fs::exists(path));
        CHECK(no_temps(path));
    }
    SECTION("nonexistent directory") {
        const std::string path = (tmp.path / "missing" / "cavity.glb").string();
        const std::string msg = thrown_message([&] { write_glb(path, m, cfg); });
        CHECK(msg == "cannot write " + path + ": " + std::strerror(ENOENT));
        CHECK(no_temps(path));
    }
    SECTION("the rename fails: the temporary is removed") {
        // The target is a non-empty directory: the temporary is written, then rename() refuses.
        const fs::path target = tmp.path / "out.glb";
        fs::create_directory(target);
        std::ofstream(target / "keep") << "x";
        const std::string path = target.string();
        const std::string msg = thrown_message([&] { write_glb(path, m, cfg); });
        CHECK(msg.rfind("cannot write " + path + ": ", 0) == 0);
        CHECK(msg.size() > ("cannot write " + path + ": ").size());
        CHECK(no_temps(path));
        CHECK(fs::is_directory(target));
        CHECK(fs::exists(target / "keep"));
    }
}

TEST_CASE("glb_size_limit_error", "[recon][glb]") {
    // A full-size 6072 x 4042 master at decimate 1, dust mesh included, is sized arithmetically
    // (nothing allocated) and fits: 24 543 024 vertices x (24 + 28) B + 24 532 911 cells x 24 B.
    const std::uint64_t v = 6072ULL * 4042ULL, cells = 6071ULL * 4041ULL;
    const std::uint64_t bin = v * 52ULL + cells * 24ULL;
    CHECK(glb_size_bytes(6072, 4042, 1, true, 0) == 12 + 8 + 8 + bin);
    CHECK(glb_size_bytes(6072, 4042, 1, true) == 12 + 8 + GLB_JSON_RESERVE + 8 + bin);
    CHECK(glb_size_bytes(6072, 4042, 1, true) <= GLB_MAX_BYTES);
    CHECK_NOTHROW(glb_check_size(6072, 4042, 1, true));
    CHECK(glb_size_bytes(6072, 4042, 1, false, 0) == 12 + 8 + 8 + v * 24ULL + cells * 24ULL);
    CHECK(glb_size_bytes(6072, 4042, 1, false, 5) == 12 + 8 + 8 + 8 + v * 24ULL + cells * 24ULL);  // JSON padded

    // Over the limit: 20000 x 20000 at decimate 1 (~20.8 GB); decimate 3 brings it under.
    CHECK(glb_size_bytes(20000, 20000, 1, true) > GLB_MAX_BYTES);
    CHECK_THROWS_AS(glb_check_size(20000, 20000, 1, true), std::length_error);
    CHECK(thrown_message([] { glb_check_size(20000, 20000, 1, true); }) ==
          "GLB would exceed 4 GiB; raise glbDecimate (now 1)");
    CHECK_NOTHROW(glb_check_size(20000, 20000, 3, true));

    // Dimensions whose byte count overflows 64 bits saturate rather than wrap to a small size.
    CHECK(glb_size_bytes(INT_MAX, INT_MAX, 1, true) == std::numeric_limits<std::uint64_t>::max());
    CHECK(thrown_message([] { glb_check_size(INT_MAX, INT_MAX, 1, true); }) ==
          "GLB would exceed 4 GiB; raise glbDecimate (now 1)");
    CHECK(thrown_message([] { glb_check_size(INT_MAX, INT_MAX, 7, false); }) ==
          "GLB would exceed 4 GiB; raise glbDecimate (now 7)");
}

TEST_CASE("glb_concurrent_calls_identical", "[recon][glb]") {
    CavityModel m = blister_model();
    m.pc_per_px = 0.03;
    for (int y = 40; y < 50; ++y)
        for (int x = 20; x < 60; ++x) m.dust.at(x, y) = 0.3;  // the dust mesh too
    for (const int d : {1, 2}) {
        const ExportConfig cfg = with_decimate(d);
        std::vector<std::uint8_t> a, b;
        std::thread t1([&] { a = build_glb(m, cfg); });
        std::thread t2([&] { b = build_glb(m, cfg); });
        t1.join();
        t2.join();
        REQUIRE_FALSE(a.empty());
        CHECK(a == b);
        CHECK(a == build_glb(m, cfg));
        CHECK(parse_glb(a).json["accessors"].arr.size() == 5);
    }
}
