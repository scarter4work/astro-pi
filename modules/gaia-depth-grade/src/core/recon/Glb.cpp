#include "Glb.h"

#include "Cavity.h"
#include "Reconstruct.h"
#include "core/Adql.h"
#include "core/AtomicFile.h"
#include "core/Parallel.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace gdg::recon {

static_assert(std::endian::native == std::endian::little, "GLB is little-endian; the writer copies host words");

namespace {

constexpr std::uint64_t kSat = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint32_t kMagic = 0x46546C67;      // "glTF"
constexpr std::uint32_t kChunkJson = 0x4E4F534A;  // "JSON"
constexpr std::uint32_t kChunkBin = 0x004E4942;   // "BIN\0"
constexpr int kFloat = 5126, kUint32 = 5125;
constexpr int kArrayBuffer = 34962, kElementArrayBuffer = 34963;

// Per-vertex bytes: position + colour (surface), position + RGBA colour (dust).
constexpr std::uint64_t kSurfaceVertexBytes = 12 + 12;
constexpr std::uint64_t kDustVertexBytes = 12 + 16;
constexpr std::uint64_t kCellIndexBytes = 6 * 4;

std::uint64_t sat_mul(std::uint64_t a, std::uint64_t b) { return a != 0 && b > kSat / a ? kSat : a * b; }
std::uint64_t sat_add(std::uint64_t a, std::uint64_t b) { return b > kSat - a ? kSat : a + b; }
std::uint64_t pad4(std::uint64_t n) { return n > kSat - 3 ? kSat : (n + 3) & ~std::uint64_t{3}; }

/// A JSON string literal: quotes, backslashes and control characters escaped (UTF-8 passes as is).
std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    return out + "\"";
}

/// A finite JSON number in shortest round-trip form (py_float_repr throws on non-finite).
std::string num(double v) { return gdg::py_float_repr(v); }
std::string num(std::uint64_t v) { return std::to_string(v); }

void put_u32(std::uint8_t* p, std::uint32_t v) { std::memcpy(p, &v, 4); }
void put_f32(std::uint8_t* p, float v) { std::memcpy(p, &v, 4); }

struct Bounds {
    float lo[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                   std::numeric_limits<float>::infinity()};
    float hi[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                   -std::numeric_limits<float>::infinity()};
    void add(float x, float y, float z) {
        const float v[3] = {x, y, z};
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], v[k]);
            hi[k] = std::max(hi[k], v[k]);
        }
    }
    void add(const Bounds& o) {
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], o.lo[k]);
            hi[k] = std::max(hi[k], o.hi[k]);
        }
    }
    std::string json() const {
        return "\"min\":[" + num(double{lo[0]}) + "," + num(double{lo[1]}) + "," + num(double{lo[2]}) + "],\"max\":[" +
               num(double{hi[0]}) + "," + num(double{hi[1]}) + "," + num(double{hi[2]}) + "]";
    }
};

/// One full-resolution row of the model: what R, has_dust and the finiteness check need.
struct RowScan {
    double r_pos = -1.0;  ///< largest apex distance of a pixel with z > 0 (-1: none)
    double r_all = 0.0;   ///< largest apex distance of any pixel
    double dust_max = -std::numeric_limits<double>::infinity();
    int bad_x = -1;       ///< first column holding a non-finite z / emissivity / dust value
};

void require(bool ok, const std::string& what) {
    if (!ok) throw std::invalid_argument("GLB: " + what);
}

void require_finite_meta(const CavityMeta& m) {
    for (const double v : {m.w_geometry, m.w_ionization_max, m.w_ionization_effective, m.w_emission, m.emission_sign,
                           m.w_dust, m.ionization_confidence, m.ratio_confidence, m.footprint_frac})
        require(std::isfinite(v), "the cavity meta holds a non-finite value");
}

}  // namespace

std::uint64_t glb_size_bytes(int nx, int ny, int decimate, bool has_dust, std::uint64_t json_bytes) {
    if (nx < 1 || ny < 1 || decimate < 1) throw std::invalid_argument("glb_size_bytes: bad grid dimensions");
    const std::uint64_t cols = static_cast<std::uint64_t>(glb_grid_count(nx, decimate));
    const std::uint64_t rows = static_cast<std::uint64_t>(glb_grid_count(ny, decimate));
    const std::uint64_t verts = cols * rows;  // < 2^62
    const std::uint64_t cells = (cols - 1) * (rows - 1);
    std::uint64_t bin = sat_add(sat_mul(verts, kSurfaceVertexBytes), sat_mul(cells, kCellIndexBytes));
    if (has_dust) bin = sat_add(bin, sat_mul(verts, kDustVertexBytes));
    // header 12, JSON chunk header 8 + padded JSON, BIN chunk header 8 + BIN (a multiple of 4)
    return sat_add(sat_add(28, pad4(json_bytes)), pad4(bin));
}

void glb_check_size(int nx, int ny, int decimate, bool has_dust, std::uint64_t json_bytes) {
    if (glb_size_bytes(nx, ny, decimate, has_dust, json_bytes) > GLB_MAX_BYTES)
        throw std::length_error("GLB would exceed 4 GiB; raise glbDecimate (now " + std::to_string(decimate) + ")");
}

std::vector<std::uint8_t> build_glb(const CavityModel& model, const ExportConfig& cfg) {
    // ---- validation (nothing allocated) ----
    const int d = cfg.decimate;
    require(d >= 1, "glbDecimate must be >= 1 (now " + std::to_string(d) + ")");
    require(std::isfinite(cfg.depth_ratio) && cfg.depth_ratio >= 0.0, "depth_ratio must be finite and >= 0");
    require(std::isfinite(cfg.dust_offset_frac), "dust_offset_frac must be finite");
    require(std::isfinite(cfg.dust_min), "dust_min must be finite");
    const Image& zi = model.z;
    const Image& ei = model.emissivity;
    const Image& di = model.dust;
    require(zi.channels == 1 && di.channels == 1 && ei.channels == 3,
            "z and dust must be mono and emissivity 3-channel");
    if (model.pc_per_px)
        require(std::isfinite(*model.pc_per_px) && *model.pc_per_px > 0.0,
                "pc_per_px must be a positive finite scale when present");
    const double ax = model.apex_xy.first, ay = model.apex_xy.second;
    require(std::isfinite(ax) && std::isfinite(ay), "the apex must be finite");
    require_finite_meta(model.meta);

    const int nx = zi.width, ny = zi.height;
    const int cols = glb_grid_count(nx, d), rows = glb_grid_count(ny, d);
    require(cols >= 2 && rows >= 2, "a " + std::to_string(nx) + "x" + std::to_string(ny) + " model at glbDecimate " +
                                        std::to_string(d) + " gives a " + std::to_string(cols) + "x" +
                                        std::to_string(rows) + " grid; at least 2x2 vertices are needed");
    // Even without the dust mesh and the JSON the file cannot fit: fail before any allocation.
    glb_check_size(nx, ny, d, false, 0);

    // ---- full-resolution scan: R, max(dust), finiteness (parallel by rows) ----
    std::vector<RowScan> scan(static_cast<size_t>(ny));
    gdg::parallel_for_dynamic(ny, std::max(1, 65536 / std::max(nx, 1)), [&](int y, int) {
        RowScan s;
        for (int x = 0; x < nx; ++x) {
            const double zv = zi.at(x, y), dv = di.at(x, y);
            if (s.bad_x < 0 && !(std::isfinite(zv) && std::isfinite(dv) && std::isfinite(ei.at(x, y, 0)) &&
                                 std::isfinite(ei.at(x, y, 1)) && std::isfinite(ei.at(x, y, 2))))
                s.bad_x = x;
            const double r = std::hypot(x - ax, y - ay);
            s.r_all = std::max(s.r_all, r);
            if (zv > 0.0) s.r_pos = std::max(s.r_pos, r);
            s.dust_max = std::max(s.dust_max, dv);
        }
        scan[static_cast<size_t>(y)] = s;
    });
    double r_pos = -1.0, r_all = 0.0, dust_max = -std::numeric_limits<double>::infinity();
    for (int y = 0; y < ny; ++y) {
        const RowScan& s = scan[static_cast<size_t>(y)];
        require(s.bad_x < 0, "the cavity model holds a non-finite value at (" + std::to_string(s.bad_x) + ", " +
                                 std::to_string(y) + ")");
        r_pos = std::max(r_pos, s.r_pos);
        r_all = std::max(r_all, s.r_all);
        dust_max = std::max(dust_max, s.dust_max);
    }
    const double R = r_pos >= 0.0 ? r_pos : r_all;
    const double D = cfg.depth_ratio * R;
    const double u = model.pc_per_px ? *model.pc_per_px : 1.0;
    const double dust_off = cfg.dust_offset_frac * D * u;
    const bool has_dust = dust_max >= cfg.dust_min;

    // Grid vertex (c, r) -> float32 position; Zd is the dust layer's Z.
    struct Vertex {
        float X, Y, Zs, Zd;
    };
    auto vertex = [&](int c, int r) {
        const int x = c * d, y = r * d;
        const double zs = zi.at(x, y) * D * u;
        return Vertex{static_cast<float>((x - ax) * u), static_cast<float>((ay - y) * u),
                      static_cast<float>(0.0 - zs), static_cast<float>(dust_off - zs)};
    };

    // ---- accessor bounds (parallel by grid rows, reduced in row order) ----
    std::vector<Bounds> row_surf(static_cast<size_t>(rows)), row_dust(static_cast<size_t>(rows));
    gdg::parallel_for_dynamic(rows, std::max(1, 16384 / cols), [&](int r, int) {
        Bounds s, dd;
        for (int c = 0; c < cols; ++c) {
            const Vertex v = vertex(c, r);
            s.add(v.X, v.Y, v.Zs);
            dd.add(v.X, v.Y, v.Zd);
        }
        row_surf[static_cast<size_t>(r)] = s;
        row_dust[static_cast<size_t>(r)] = dd;
    });
    Bounds surf, dust;
    for (int r = 0; r < rows; ++r) {
        surf.add(row_surf[static_cast<size_t>(r)]);
        dust.add(row_dust[static_cast<size_t>(r)]);
    }

    // ---- BIN layout ----
    const std::uint64_t V = static_cast<std::uint64_t>(cols) * static_cast<std::uint64_t>(rows);
    const std::uint64_t I = 6ULL * static_cast<std::uint64_t>(cols - 1) * static_cast<std::uint64_t>(rows - 1);
    const std::uint64_t o_pos = 0, o_col = o_pos + 12 * V, o_idx = o_col + 12 * V, o_dpos = o_idx + 4 * I,
                        o_dcol = o_dpos + 12 * V;
    const std::uint64_t bin_len = has_dust ? o_dcol + 16 * V : o_dpos;

    // ---- JSON ----
    const std::string unlit = "\"extensions\":{\"KHR_materials_unlit\":{}}";
    const std::string pbr =
        "\"pbrMetallicRoughness\":{\"baseColorFactor\":[1.0,1.0,1.0,1.0],\"metallicFactor\":0.0,\"roughnessFactor\":1.0}";
    auto view = [&](std::uint64_t off, std::uint64_t len, int target) {
        return "{\"buffer\":0,\"byteOffset\":" + num(off) + ",\"byteLength\":" + num(len) +
               ",\"target\":" + std::to_string(target) + "}";
    };
    auto accessor = [&](int view_index, int ctype, std::uint64_t count, const char* type, const std::string& extra) {
        return "{\"bufferView\":" + std::to_string(view_index) + ",\"byteOffset\":0,\"componentType\":" +
               std::to_string(ctype) + ",\"count\":" + num(count) + ",\"type\":\"" + type + "\"" +
               (extra.empty() ? "" : "," + extra) + "}";
    };
    auto primitive = [](int pos, int col, int material) {
        return "{\"attributes\":{\"POSITION\":" + std::to_string(pos) + ",\"COLOR_0\":" + std::to_string(col) +
               "},\"indices\":2,\"material\":" + std::to_string(material) + ",\"mode\":4}";
    };

    std::string js;
    js += "{\"asset\":{\"version\":\"2.0\",\"generator\":" + json_str(GLB_GENERATOR) +
          ",\"extras\":{\"honesty\":" + json_str(RECON_HONESTY) + ",\"units\":" +
          json_str(model.pc_per_px ? "pc" : "px") + ",\"depth_ratio\":" + num(cfg.depth_ratio) +
          ",\"meta\":" + cavity_meta_json(model.meta) + "}}";
    js += ",\"extensionsUsed\":[\"KHR_materials_unlit\"]";
    js += ",\"scene\":0,\"scenes\":[{\"nodes\":" + std::string(has_dust ? "[0,1]" : "[0]") + "}]";
    js += ",\"nodes\":[{\"name\":\"cavity\",\"mesh\":0}" + std::string(has_dust ? ",{\"name\":\"dust\",\"mesh\":1}" : "") +
          "]";
    js += ",\"meshes\":[{\"name\":\"cavity\",\"primitives\":[" + primitive(0, 1, 0) + "]}";
    if (has_dust) js += ",{\"name\":\"dust\",\"primitives\":[" + primitive(3, 4, 1) + "]}";
    js += "]";
    js += ",\"materials\":[{\"name\":\"cavity\"," + pbr + ",\"doubleSided\":true," + unlit + "}";
    if (has_dust) js += ",{\"name\":\"dust\"," + pbr + ",\"alphaMode\":\"BLEND\",\"doubleSided\":true," + unlit + "}";
    js += "]";
    js += ",\"buffers\":[{\"byteLength\":" + num(bin_len) + "}]";
    js += ",\"bufferViews\":[" + view(o_pos, 12 * V, kArrayBuffer) + "," + view(o_col, 12 * V, kArrayBuffer) + "," +
          view(o_idx, 4 * I, kElementArrayBuffer);
    if (has_dust) js += "," + view(o_dpos, 12 * V, kArrayBuffer) + "," + view(o_dcol, 16 * V, kArrayBuffer);
    js += "]";
    js += ",\"accessors\":[" + accessor(0, kFloat, V, "VEC3", surf.json()) + "," +
          accessor(1, kFloat, V, "VEC3", "") + "," + accessor(2, kUint32, I, "SCALAR", "");
    if (has_dust) js += "," + accessor(3, kFloat, V, "VEC3", dust.json()) + "," + accessor(4, kFloat, V, "VEC4", "");
    js += "]}";

    // ---- exact size check, then the one large allocation ----
    glb_check_size(nx, ny, d, has_dust, js.size());
    const std::uint64_t json_len = pad4(js.size());
    const std::uint64_t total = glb_size_bytes(nx, ny, d, has_dust, js.size());
    std::vector<std::uint8_t> out(static_cast<size_t>(total), 0);
    std::uint8_t* p = out.data();
    put_u32(p, kMagic);
    put_u32(p + 4, 2);
    put_u32(p + 8, static_cast<std::uint32_t>(total));
    put_u32(p + 12, static_cast<std::uint32_t>(json_len));
    put_u32(p + 16, kChunkJson);
    std::memcpy(p + 20, js.data(), js.size());
    std::memset(p + 20 + js.size(), ' ', static_cast<size_t>(json_len - js.size()));
    std::uint8_t* chunk = p + 20 + json_len;
    put_u32(chunk, static_cast<std::uint32_t>(bin_len));
    put_u32(chunk + 4, kChunkBin);
    std::uint8_t* bin = chunk + 8;  // zero-initialised: any padding stays 0

    // ---- BIN fill, parallel by grid rows (each row owns disjoint byte ranges) ----
    const float dust_rgb[3] = {0.05f, 0.04f, 0.03f};
    gdg::parallel_for_dynamic(rows, std::max(1, 16384 / cols), [&](int r, int) {
        const int y = r * d;
        for (int c = 0; c < cols; ++c) {
            const int x = c * d;
            const std::uint64_t v = static_cast<std::uint64_t>(r) * static_cast<std::uint64_t>(cols) +
                                    static_cast<std::uint64_t>(c);
            const Vertex vx = vertex(c, r);
            std::uint8_t* pp = bin + o_pos + 12 * v;
            put_f32(pp, vx.X);
            put_f32(pp + 4, vx.Y);
            put_f32(pp + 8, vx.Zs);
            std::uint8_t* pc = bin + o_col + 12 * v;
            for (int k = 0; k < 3; ++k) put_f32(pc + 4 * k, static_cast<float>(clip(ei.at(x, y, k), 0.0, 1.0)));
            if (has_dust) {
                std::uint8_t* dp = bin + o_dpos + 12 * v;
                put_f32(dp, vx.X);
                put_f32(dp + 4, vx.Y);
                put_f32(dp + 8, vx.Zd);
                std::uint8_t* dc = bin + o_dcol + 16 * v;
                for (int k = 0; k < 3; ++k) put_f32(dc + 4 * k, dust_rgb[k]);
                put_f32(dc + 12, static_cast<float>(clip(di.at(x, y), 0.0, 1.0)));
            }
        }
        if (r + 1 < rows) {
            // Two triangles per cell, counter-clockwise seen from +Z (+Y up = image row decreasing):
            // a = (c, r) top-left, b = (c+1, r), e = (c, r+1) below a, f = (c+1, r+1).
            std::uint8_t* ip = bin + o_idx + 24ULL * static_cast<std::uint64_t>(r) * static_cast<std::uint64_t>(cols - 1);
            for (int c = 0; c + 1 < cols; ++c, ip += 24) {
                const auto a = static_cast<std::uint32_t>(r * cols + c);
                const std::uint32_t b = a + 1, e = a + static_cast<std::uint32_t>(cols), f = e + 1;
                const std::uint32_t tri[6] = {a, e, f, a, f, b};
                std::memcpy(ip, tri, sizeof tri);
            }
        }
    });
    return out;
}

void write_glb(const std::string& path, const CavityModel& model, const ExportConfig& cfg) {
    gdg::write_file_atomic(path, build_glb(model, cfg));
}

}  // namespace gdg::recon
