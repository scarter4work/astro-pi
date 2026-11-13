// The recon cavity model as a rotatable glTF 2.0 binary (.glb): recon Part 2, native (PCL-free).
//
// Layout contract (the tests and the PixInsight process rely on it):
// - Grid: columns x = i*d for i = 0 .. (nx - 1)/d, rows y = j*d likewise (integer division);
//   vertex index = row*cols + col. Row 0 is the image's top row (frame C1).
// - POSITION float32 VEC3 (accessor min/max set): X = (x - ax) u, Y = -(y - ay) u, Z = -z D u, with
//   (ax, ay) = apex_xy, D = depth_ratio * R, R = cavity_radius_px(): the largest apex distance of a
//   pixel with z > 0 (of any pixel when none), u = pc_per_px (1 when absent). +Y is up, the viewer looks down -Z.
// - COLOR_0 float32 VEC3 = clip(emissivity, 0, 1) (linear, as glTF vertex colours are).
// - Indices uint32, two triangles per cell, counter-clockwise seen from +Z.
// - Material 0: KHR_materials_unlit, doubleSided.
// - Dust mesh only when max(dust) >= dust_min: the same grid at Z + dust_offset_frac D u (nearer the
//   viewer), COLOR_0 float32 VEC4 (0.05, 0.04, 0.03, clip(dust, 0, 1)), material 1 (unlit,
//   alphaMode BLEND, doubleSided); it reuses the index accessor.
// - bufferViews and accessors in this order: surface positions, surface colours, indices, then dust
//   positions and dust colours.
// - asset = {"version":"2.0","generator":"GaiaDepthGrade NebulaReconstruct","extras":{"honesty":
//   RECON_HONESTY,"units":"pc"|"px","depth_ratio":...,"meta":<cavity_meta_json>}}.
// - JSON chunk space-padded and BIN chunk zero-padded to 4 bytes; magic "glTF", version 2.
#pragma once

#include "Model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gdg::recon {

struct ExportConfig {
    int decimate = 2;               ///< grid step in pixels (glbDecimate); 2 is the Python plan's default
    double depth_ratio = 0.5;       ///< depth scale D as a fraction of the cavity radius R
    double dust_offset_frac = 0.02; ///< dust layer offset toward the viewer, as a fraction of D
    double dust_min = 0.01;         ///< no dust mesh unless max(dust) reaches this opacity
};

inline constexpr const char* GLB_GENERATOR = "GaiaDepthGrade NebulaReconstruct";

/// The JSON chunk allowance glb_size_bytes() counts when the caller does not know the JSON length.
/// The real JSON chunk is a few kB (accessor bounds, the cavity meta).
inline constexpr std::uint64_t GLB_JSON_RESERVE = 16384;

/// glTF's binary container stores lengths as uint32.
inline constexpr std::uint64_t GLB_MAX_BYTES = 4294967295ULL;

/// Grid vertex counts along one axis for an image of `n` pixels at step `decimate`: (n - 1)/d + 1.
inline int glb_grid_count(int n, int decimate) { return n < 1 || decimate < 1 ? 0 : (n - 1) / decimate + 1; }

/// The size in bytes of the GLB build_glb() writes for an nx x ny model at `decimate`, with or
/// without the dust mesh, counting the JSON chunk as `json_bytes` (padded to 4). Exact when
/// `json_bytes` is the real JSON length; an upper bound with the default reserve. Pure arithmetic:
/// allocates nothing, and saturates at UINT64_MAX instead of overflowing. nx, ny or decimate < 1
/// throws std::invalid_argument.
std::uint64_t glb_size_bytes(int nx, int ny, int decimate, bool has_dust,
                             std::uint64_t json_bytes = GLB_JSON_RESERVE);

/// Throws std::length_error("GLB would exceed 4 GiB; raise glbDecimate (now <d>)") when
/// glb_size_bytes(nx, ny, decimate, has_dust, json_bytes) > GLB_MAX_BYTES. build_glb() calls it
/// twice: right after validation as a lower bound (no dust, no JSON) before allocating anything,
/// then with the exact JSON length before allocating the file.
void glb_check_size(int nx, int ny, int decimate, bool has_dust, std::uint64_t json_bytes = GLB_JSON_RESERVE);

/// The cavity radius R of the layout contract: the largest distance from apex_xy of a pixel with
/// z > 0, or of any pixel when none has one (0 for an empty model). The depth scale of the GLB and
/// of the orbit animation (Orbit.h) is D = depth_ratio * R. Throws std::invalid_argument unless z
/// is a plane and the apex is finite. Reentrant: parallel by rows, exact (a max reduction).
double cavity_radius_px(const CavityModel& model);

/// The cavity model as GLB bytes (layout above). Throws std::invalid_argument for decimate < 1, a
/// grid under 2 x 2 vertices, non-finite config or model values, a pc_per_px <= 0, or planes of the
/// wrong channel count, or an apex more than its own size outside the grid (Validate.h);
/// std::length_error (glb_check_size) when the file would exceed 4 GiB.
/// Reentrant: the BIN chunk is filled in parallel by rows, with bit-identical output.
std::vector<std::uint8_t> build_glb(const CavityModel& model, const ExportConfig& cfg);

/// build_glb() written atomically to `path` (gdg::write_file_atomic: a unique `<path>.tmp.XXXXXX`,
/// fsync, rename, directory fsync). A write failure throws std::runtime_error("cannot write <path>:
/// <strerror>") and leaves no temporary file.
void write_glb(const std::string& path, const CavityModel& model, const ExportConfig& cfg);

}  // namespace gdg::recon
