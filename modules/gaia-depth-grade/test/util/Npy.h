// Minimal NumPy .npy reader for the golden files (test tooling, header only, PCL-free).
//
// Reads format versions 1.0, 2.0 and 3.0, C-order only, little-endian or byte-size-1 dtypes:
//   <f8  <f4  <i8  <i4  |b1  |u1  |i1
// Every element is widened to double, which is exact for all of these types except i8 values
// beyond 2^53 (the goldens carry none). Malformed or truncated input throws std::runtime_error.
#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace gdg::test {

struct NpyArray {
    std::vector<size_t> shape;  // empty for a 0-d scalar
    std::vector<double> data;   // C order, widened to double
};

namespace npy_detail {

inline std::runtime_error fail(const std::string& path, const std::string& why) {
    return std::runtime_error("npy: " + path + ": " + why);
}

// Value of 'key' in the header dict literal, e.g. the text after "'descr':". Returns the raw
// token (quoted string, bool, or parenthesised tuple) with surrounding blanks trimmed.
inline std::string dict_value(const std::string& path, const std::string& header, const std::string& key) {
    const std::string needle = "'" + key + "'";
    const size_t k = header.find(needle);
    if (k == std::string::npos) throw fail(path, "header has no '" + key + "'");
    size_t p = header.find(':', k + needle.size());
    if (p == std::string::npos) throw fail(path, "malformed header near '" + key + "'");
    ++p;
    while (p < header.size() && header[p] == ' ') ++p;
    if (p >= header.size()) throw fail(path, "malformed header near '" + key + "'");
    size_t e;
    if (header[p] == '\'') {
        e = header.find('\'', p + 1);
        if (e == std::string::npos) throw fail(path, "unterminated string in header");
        ++e;
    } else if (header[p] == '(') {
        e = header.find(')', p);
        if (e == std::string::npos) throw fail(path, "unterminated shape in header");
        ++e;
    } else {
        e = header.find_first_of(",}", p);
        if (e == std::string::npos) throw fail(path, "malformed header near '" + key + "'");
    }
    std::string v = header.substr(p, e - p);
    while (!v.empty() && v.back() == ' ') v.pop_back();
    return v;
}

template <class T>
inline double at(const unsigned char* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));  // host is little-endian (x86-64 / aarch64); descr is checked below
    return static_cast<double>(v);
}

}  // namespace npy_detail

inline NpyArray load_npy(const std::string& path) {
    using npy_detail::fail;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw fail(path, "cannot open");
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    static const unsigned char kMagic[6] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    if (bytes.size() < 10 || std::memcmp(bytes.data(), kMagic, 6) != 0) throw fail(path, "not an npy file (bad magic or too short)");
    const unsigned major = bytes[6];
    size_t header_len, header_at;
    if (major == 1) {
        header_len = bytes[8] | (static_cast<size_t>(bytes[9]) << 8);
        header_at = 10;
    } else if (major == 2 || major == 3) {
        if (bytes.size() < 12) throw fail(path, "truncated header");
        header_len = bytes[8] | (static_cast<size_t>(bytes[9]) << 8) | (static_cast<size_t>(bytes[10]) << 16) |
                     (static_cast<size_t>(bytes[11]) << 24);
        header_at = 12;
    } else {
        throw fail(path, "unsupported format version " + std::to_string(major));
    }
    if (bytes.size() < header_at + header_len) throw fail(path, "truncated header");
    const std::string header(reinterpret_cast<const char*>(bytes.data()) + header_at, header_len);

    const std::string descr_tok = npy_detail::dict_value(path, header, "descr");
    const std::string descr = descr_tok.substr(1, descr_tok.size() - 2);
    if (npy_detail::dict_value(path, header, "fortran_order") != "False")
        throw fail(path, "fortran_order arrays are not supported");

    // Shape tuple: "()", "(4,)", "(3, 2)".
    const std::string shape_tok = npy_detail::dict_value(path, header, "shape");
    NpyArray out;
    {
        size_t i = 1;
        while (i + 1 < shape_tok.size()) {
            while (i < shape_tok.size() && (shape_tok[i] == ' ' || shape_tok[i] == ',')) ++i;
            if (i >= shape_tok.size() || shape_tok[i] == ')') break;
            size_t used = 0;
            unsigned long long v;
            try {
                v = std::stoull(shape_tok.substr(i), &used);
            } catch (const std::exception&) {
                throw fail(path, "bad shape '" + shape_tok + "'");
            }
            out.shape.push_back(static_cast<size_t>(v));
            i += used;
        }
    }
    size_t count = 1;
    for (const size_t d : out.shape) count *= d;

    size_t item;
    enum Kind { F8, F4, I8, I4, B1, U1, I1 } kind;
    if (descr == "<f8") { item = 8; kind = F8; }
    else if (descr == "<f4") { item = 4; kind = F4; }
    else if (descr == "<i8") { item = 8; kind = I8; }
    else if (descr == "<i4") { item = 4; kind = I4; }
    else if (descr == "|b1") { item = 1; kind = B1; }
    else if (descr == "|u1") { item = 1; kind = U1; }
    else if (descr == "|i1") { item = 1; kind = I1; }
    else throw fail(path, "unsupported dtype '" + descr + "'");

    const size_t payload_at = header_at + header_len;
    if (bytes.size() - payload_at != count * item)
        throw fail(path, "payload is " + std::to_string(bytes.size() - payload_at) + " bytes, shape needs " +
                             std::to_string(count * item));

    out.data.resize(count);
    const unsigned char* p = bytes.data() + payload_at;
    for (size_t i = 0; i < count; ++i, p += item) {
        switch (kind) {
            case F8: out.data[i] = npy_detail::at<double>(p); break;
            case F4: out.data[i] = npy_detail::at<float>(p); break;
            case I8: out.data[i] = npy_detail::at<int64_t>(p); break;
            case I4: out.data[i] = npy_detail::at<int32_t>(p); break;
            case B1: out.data[i] = *p ? 1.0 : 0.0; break;
            case U1: out.data[i] = *p; break;
            case I1: out.data[i] = static_cast<int8_t>(*p); break;
        }
    }
    return out;
}

}  // namespace gdg::test
