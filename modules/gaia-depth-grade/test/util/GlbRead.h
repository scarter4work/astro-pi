// Test-only GLB reader: header and chunk parsing, and typed reads of accessor data from the BIN
// chunk. Validates what it reads (throws std::runtime_error on any structural fault) so tests can
// rely on the decoded values.
#pragma once

#include "Json.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace gdg::test {

struct GlbFile {
    std::uint32_t magic = 0, version = 0, length = 0;
    std::uint32_t json_chunk_length = 0, json_chunk_type = 0;
    std::uint32_t bin_chunk_length = 0, bin_chunk_type = 0;
    std::string json_text;  ///< the JSON chunk as stored (padding included)
    Json json;
    std::vector<std::uint8_t> bin;  ///< the BIN chunk's data (padding included)
};

inline constexpr std::uint32_t GLB_MAGIC = 0x46546C67;       // "glTF"
inline constexpr std::uint32_t GLB_CHUNK_JSON = 0x4E4F534A;  // "JSON"
inline constexpr std::uint32_t GLB_CHUNK_BIN = 0x004E4942;   // "BIN\0"

inline std::uint32_t glb_u32(const std::vector<std::uint8_t>& b, size_t off) {
    if (off + 4 > b.size()) throw std::runtime_error("GlbRead: truncated");
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

inline GlbFile parse_glb(const std::vector<std::uint8_t>& b) {
    GlbFile g;
    g.magic = glb_u32(b, 0);
    g.version = glb_u32(b, 4);
    g.length = glb_u32(b, 8);
    g.json_chunk_length = glb_u32(b, 12);
    g.json_chunk_type = glb_u32(b, 16);
    const size_t json_end = 20 + static_cast<size_t>(g.json_chunk_length);
    if (json_end > b.size()) throw std::runtime_error("GlbRead: JSON chunk overruns the file");
    g.json_text.assign(reinterpret_cast<const char*>(b.data()) + 20, g.json_chunk_length);
    g.json = Json::parse(g.json_text);
    if (json_end < b.size()) {
        g.bin_chunk_length = glb_u32(b, json_end);
        g.bin_chunk_type = glb_u32(b, json_end + 4);
        const size_t bin_begin = json_end + 8;
        if (bin_begin + g.bin_chunk_length > b.size()) throw std::runtime_error("GlbRead: BIN chunk overruns the file");
        g.bin.assign(b.begin() + static_cast<std::ptrdiff_t>(bin_begin),
                     b.begin() + static_cast<std::ptrdiff_t>(bin_begin + g.bin_chunk_length));
    }
    return g;
}

inline std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("GlbRead: cannot open " + path);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Components per element of a glTF accessor type.
inline int glb_type_components(const std::string& type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    throw std::runtime_error("GlbRead: unexpected accessor type " + type);
}

template <typename T> constexpr int glb_component_type();
template <> constexpr int glb_component_type<float>() { return 5126; }
template <> constexpr int glb_component_type<std::uint32_t>() { return 5125; }

/// Accessor `index` read as a flat T array (count * components values). Checks the accessor's
/// componentType against T and that the data lies inside its bufferView and the BIN chunk.
template <typename T>
std::vector<T> read_accessor(const GlbFile& g, size_t index) {
    const Json& acc = g.json["accessors"][index];
    if (static_cast<int>(acc["componentType"].num) != glb_component_type<T>())
        throw std::runtime_error("GlbRead: componentType mismatch");
    const size_t count = static_cast<size_t>(acc["count"].num);
    const size_t comps = static_cast<size_t>(glb_type_components(acc["type"].str));
    const Json& view = g.json["bufferViews"][static_cast<size_t>(acc["bufferView"].num)];
    const size_t view_off = view.has("byteOffset") ? static_cast<size_t>(view["byteOffset"].num) : 0;
    const size_t view_len = static_cast<size_t>(view["byteLength"].num);
    const size_t acc_off = acc.has("byteOffset") ? static_cast<size_t>(acc["byteOffset"].num) : 0;
    if (view.has("byteStride") && static_cast<size_t>(view["byteStride"].num) != comps * sizeof(T))
        throw std::runtime_error("GlbRead: interleaved views are not supported");
    const size_t bytes = count * comps * sizeof(T);
    if (acc_off + bytes > view_len || view_off + view_len > g.bin.size())
        throw std::runtime_error("GlbRead: accessor overruns its bufferView");
    std::vector<T> out(count * comps);
    std::memcpy(out.data(), g.bin.data() + view_off + acc_off, bytes);  // little-endian host
    return out;
}

}  // namespace gdg::test
