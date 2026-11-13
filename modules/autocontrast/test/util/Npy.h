// Readers for the golden files written by test/golden/make_goldens.py.
//
//   load_npy   NumPy .npy v1/v2, C order, dtypes <f8 <f4 <i8 <u2 |u1 |b1,
//              every element widened to double (u2/u1/b1/f4/i<=2^53 are exact).
//   load_json  nlohmann::json from a file.
//
// Both throw std::runtime_error on a missing, malformed or truncated file; a golden
// that cannot be read must fail the test loudly, never produce an empty array.
#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace ac::test {

struct NpyArray {
    std::vector<size_t> shape;
    std::vector<double> data;
    std::string dtype;

    size_t size() const { return data.size(); }
};

namespace detail {

inline std::string npy_fail(const std::string& path, const std::string& why) {
    return "load_npy(" + path + "): " + why;
}

template <typename T>
inline T read_le(const unsigned char* p) {
    // Host is little-endian x86-64/aarch64; the supported dtypes are all little-endian
    // or single-byte, so a raw copy is the decode.
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

}  // namespace detail

inline NpyArray load_npy(const std::string& path) {
    using detail::npy_fail;
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(npy_fail(path, "cannot open"));
    const std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());

    static const unsigned char magic[6] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    if (buf.size() < 10 || std::memcmp(buf.data(), magic, 6) != 0)
        throw std::runtime_error(npy_fail(path, "not an .npy file (bad magic or too short)"));

    const unsigned major = buf[6];
    size_t header_len = 0, header_off = 0;
    if (major == 1) {
        header_len = buf[8] | (static_cast<size_t>(buf[9]) << 8);
        header_off = 10;
    } else if (major == 2 || major == 3) {
        if (buf.size() < 12) throw std::runtime_error(npy_fail(path, "truncated header"));
        header_len = static_cast<size_t>(detail::read_le<uint32_t>(&buf[8]));
        header_off = 12;
    } else {
        throw std::runtime_error(npy_fail(path, "unsupported .npy version"));
    }
    if (buf.size() < header_off + header_len)
        throw std::runtime_error(npy_fail(path, "truncated header"));
    const std::string header(reinterpret_cast<const char*>(&buf[header_off]), header_len);

    // Header is a Python dict literal: {'descr': '<f8', 'fortran_order': False, 'shape': (3, 2), }
    auto value_after = [&](const std::string& key) -> size_t {
        const size_t k = header.find("'" + key + "'");
        if (k == std::string::npos)
            throw std::runtime_error(npy_fail(path, "header lacks '" + key + "'"));
        const size_t colon = header.find(':', k);
        if (colon == std::string::npos)
            throw std::runtime_error(npy_fail(path, "malformed header"));
        size_t i = colon + 1;
        while (i < header.size() && header[i] == ' ') ++i;
        return i;
    };

    NpyArray out;
    {
        size_t i = value_after("descr");
        if (i >= header.size() || header[i] != '\'')
            throw std::runtime_error(npy_fail(path, "malformed descr"));
        const size_t e = header.find('\'', i + 1);
        if (e == std::string::npos) throw std::runtime_error(npy_fail(path, "malformed descr"));
        out.dtype = header.substr(i + 1, e - i - 1);
    }
    {
        const size_t i = value_after("fortran_order");
        if (header.compare(i, 5, "False") != 0)
            throw std::runtime_error(npy_fail(path, "Fortran order is not supported"));
    }
    {
        size_t i = value_after("shape");
        if (i >= header.size() || header[i] != '(')
            throw std::runtime_error(npy_fail(path, "malformed shape"));
        const size_t e = header.find(')', i);
        if (e == std::string::npos) throw std::runtime_error(npy_fail(path, "malformed shape"));
        std::string inner = header.substr(i + 1, e - i - 1);
        size_t p = 0;
        while (p < inner.size()) {
            while (p < inner.size() && (inner[p] == ' ' || inner[p] == ',')) ++p;
            if (p >= inner.size()) break;
            size_t used = 0;
            const unsigned long long v = std::stoull(inner.substr(p), &used);
            out.shape.push_back(static_cast<size_t>(v));
            p += used;
        }
    }

    size_t count = 1;
    for (size_t d : out.shape) count *= d;  // () -> 1 (scalar)

    size_t item = 0;
    if (out.dtype == "<f8" || out.dtype == "<i8") item = 8;
    else if (out.dtype == "<f4") item = 4;
    else if (out.dtype == "<u2") item = 2;
    else if (out.dtype == "|u1" || out.dtype == "|b1") item = 1;
    else throw std::runtime_error(npy_fail(path, "unsupported dtype " + out.dtype));

    const size_t data_off = header_off + header_len;
    if (buf.size() < data_off + count * item)
        throw std::runtime_error(npy_fail(path, "truncated data"));
    if (buf.size() != data_off + count * item)
        throw std::runtime_error(npy_fail(path, "trailing bytes after data"));

    out.data.resize(count);
    const unsigned char* p = buf.data() + data_off;
    for (size_t n = 0; n < count; ++n, p += item) {
        if (out.dtype == "<f8") out.data[n] = detail::read_le<double>(p);
        else if (out.dtype == "<f4") out.data[n] = static_cast<double>(detail::read_le<float>(p));
        else if (out.dtype == "<i8") out.data[n] = static_cast<double>(detail::read_le<int64_t>(p));
        else if (out.dtype == "<u2") out.data[n] = static_cast<double>(detail::read_le<uint16_t>(p));
        else out.data[n] = static_cast<double>(*p);
    }
    return out;
}

inline nlohmann::json load_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("load_json(" + path + "): cannot open");
    try {
        return nlohmann::json::parse(f);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("load_json(" + path + "): " + e.what());
    }
}

}  // namespace ac::test
