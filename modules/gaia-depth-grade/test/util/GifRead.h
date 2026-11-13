// Test-only GIF87a/89a reader: block structure, extensions and a classic LZW decoder (prefix /
// suffix chains, the KwKwK case, deferred clear at 4096). Written from the GIF89a specification
// independently of src/core/Gif.cpp; it validates everything it reads and throws
// std::runtime_error on any structural fault, so tests can rely on the decoded pixels. The
// encoder is also cross-checked against ffmpeg and ImageMagick during development (task report).
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace gdg::test {

struct GifFrame {
    int delay_cs = -1;      ///< from this frame's graphic control extension (-1: none)
    int gce_count = 0;      ///< graphic control extensions seen since the previous image
    int disposal = -1;
    bool transparent = false;
    int left = 0, top = 0, width = 0, height = 0;
    bool interlaced = false;
    bool local_table = false;
    int min_code_size = 0;
    int clear_codes = 0;     ///< clear codes in the LZW stream, the leading one included
    int max_code_width = 0;  ///< widest code read
    int max_sub_block = 0;   ///< longest data sub-block
    std::vector<std::uint8_t> index;  ///< width * height palette indices
    std::vector<std::uint8_t> rgb;    ///< width * height * 3, through the active colour table
};

struct GifFile {
    std::string signature;  ///< "GIF89a" or "GIF87a"
    int width = 0, height = 0;
    int lsd_packed = 0, background = 0, aspect = 0;
    std::vector<std::uint8_t> global_table;  ///< 3 bytes per entry
    int loops = -1;               ///< NETSCAPE2.0 loop count (-1: no such extension)
    int netscape_count = 0;
    std::vector<GifFrame> frames;
    bool trailer = false;
    std::size_t trailing_bytes = 0;  ///< bytes after the trailer
};

namespace gif_detail {

struct Cursor {
    const std::vector<std::uint8_t>& b;
    std::size_t p = 0;
    std::uint8_t u8() {
        if (p >= b.size()) throw std::runtime_error("GifRead: truncated");
        return b[p++];
    }
    int u16() {
        const int lo = u8();
        return lo | (u8() << 8);
    }
    /// Concatenated data sub-blocks up to the 0 terminator.
    std::vector<std::uint8_t> sub_blocks(int* longest = nullptr) {
        std::vector<std::uint8_t> out;
        for (;;) {
            const int n = u8();
            if (n == 0) return out;
            if (longest && n > *longest) *longest = n;
            for (int i = 0; i < n; ++i) out.push_back(u8());
        }
    }
};

/// Decodes `count` indices from one image's LZW data.
inline std::vector<std::uint8_t> lzw_decode(const std::vector<std::uint8_t>& data, int min_code_size,
                                            std::size_t count, GifFrame& f) {
    if (min_code_size < 2 || min_code_size > 8) throw std::runtime_error("GifRead: bad LZW minimum code size");
    const int clear = 1 << min_code_size, eoi = clear + 1;
    std::vector<int> prefix(4096, -1);
    std::vector<std::uint8_t> suffix(4096, 0), first(4096, 0);
    std::vector<int> length(4096, 0);
    for (int c = 0; c < clear; ++c) {
        suffix[c] = static_cast<std::uint8_t>(c);
        first[c] = static_cast<std::uint8_t>(c);
        length[c] = 1;
    }
    int width = min_code_size + 1, avail = clear + 2, prev = -1;
    std::vector<std::uint8_t> out;
    out.reserve(count);
    std::size_t bitpos = 0;
    const std::size_t nbits = data.size() * 8;
    std::vector<std::uint8_t> str;
    auto emit = [&](int code) {
        str.assign(static_cast<std::size_t>(length[code]), 0);
        for (int c = code, i = length[code] - 1; c >= 0; c = prefix[c], --i) str[static_cast<std::size_t>(i)] = suffix[c];
        out.insert(out.end(), str.begin(), str.end());
    };
    bool ended = false;
    while (bitpos + static_cast<std::size_t>(width) <= nbits) {
        int code = 0;
        for (int i = 0; i < width; ++i, ++bitpos)
            code |= ((data[bitpos >> 3] >> (bitpos & 7)) & 1) << i;
        if (width > f.max_code_width) f.max_code_width = width;
        if (code == clear) {
            ++f.clear_codes;
            width = min_code_size + 1;
            avail = clear + 2;
            prev = -1;
            continue;
        }
        if (code == eoi) {
            ended = true;
            break;
        }
        if (prev < 0) {
            if (code >= clear) throw std::runtime_error("GifRead: first code after a clear is not a literal");
            emit(code);
            prev = code;
            continue;
        }
        if (code < avail) {
            emit(code);
            if (avail < 4096) {
                prefix[avail] = prev;
                suffix[avail] = first[code];
                first[avail] = first[prev];
                length[avail] = length[prev] + 1;
                ++avail;
            }
        } else if (code == avail && avail < 4096) {  // KwKwK
            prefix[avail] = prev;
            suffix[avail] = first[prev];
            first[avail] = first[prev];
            length[avail] = length[prev] + 1;
            ++avail;
            emit(code);
        } else {
            throw std::runtime_error("GifRead: LZW code " + std::to_string(code) + " not yet defined (next " +
                                     std::to_string(avail) + ")");
        }
        if (avail == (1 << width) && width < 12) ++width;
        prev = code;
        if (out.size() > count) throw std::runtime_error("GifRead: more pixels than the image holds");
    }
    if (!ended) throw std::runtime_error("GifRead: LZW data ends without an end code");
    if (out.size() != count)
        throw std::runtime_error("GifRead: " + std::to_string(out.size()) + " pixels decoded, " +
                                 std::to_string(count) + " expected");
    return out;
}

}  // namespace gif_detail

inline GifFile parse_gif(const std::vector<std::uint8_t>& bytes) {
    gif_detail::Cursor in{bytes};
    GifFile g;
    for (int i = 0; i < 6; ++i) g.signature.push_back(static_cast<char>(in.u8()));
    if (g.signature != "GIF89a" && g.signature != "GIF87a") throw std::runtime_error("GifRead: not a GIF");
    g.width = in.u16();
    g.height = in.u16();
    g.lsd_packed = in.u8();
    g.background = in.u8();
    g.aspect = in.u8();
    if (g.lsd_packed & 0x80) {
        const std::size_t n = std::size_t{3} << ((g.lsd_packed & 7) + 1);
        for (std::size_t i = 0; i < n; ++i) g.global_table.push_back(in.u8());
    }
    GifFrame pending;
    for (;;) {
        const int tag = in.u8();
        if (tag == 0x3B) {
            g.trailer = true;
            g.trailing_bytes = bytes.size() - in.p;
            break;
        }
        if (tag == 0x21) {
            const int label = in.u8();
            if (label == 0xF9) {
                if (in.u8() != 4) throw std::runtime_error("GifRead: graphic control extension size != 4");
                const int packed = in.u8();
                pending.disposal = (packed >> 2) & 7;
                pending.transparent = packed & 1;
                pending.delay_cs = in.u16();
                in.u8();  // transparent index
                if (in.u8() != 0) throw std::runtime_error("GifRead: graphic control extension not terminated");
                ++pending.gce_count;
            } else if (label == 0xFF) {
                const int n = in.u8();
                std::string app;
                for (int i = 0; i < n; ++i) app.push_back(static_cast<char>(in.u8()));
                const std::vector<std::uint8_t> data = in.sub_blocks();
                if (app == "NETSCAPE2.0") {
                    if (data.size() != 3 || data[0] != 1) throw std::runtime_error("GifRead: bad NETSCAPE2.0 block");
                    g.loops = data[1] | (data[2] << 8);
                    ++g.netscape_count;
                }
            } else {
                in.sub_blocks();  // comment / plain text: skipped
            }
            continue;
        }
        if (tag != 0x2C) throw std::runtime_error("GifRead: unexpected block 0x" + std::to_string(tag));
        GifFrame f = pending;
        pending = GifFrame{};
        f.left = in.u16();
        f.top = in.u16();
        f.width = in.u16();
        f.height = in.u16();
        const int packed = in.u8();
        f.interlaced = packed & 0x40;
        f.local_table = packed & 0x80;
        std::vector<std::uint8_t> table = g.global_table;
        if (f.local_table) {
            table.clear();
            const std::size_t n = std::size_t{3} << ((packed & 7) + 1);
            for (std::size_t i = 0; i < n; ++i) table.push_back(in.u8());
        }
        if (table.empty()) throw std::runtime_error("GifRead: no colour table for an image");
        f.min_code_size = in.u8();
        const std::vector<std::uint8_t> data = in.sub_blocks(&f.max_sub_block);
        const std::size_t count = static_cast<std::size_t>(f.width) * static_cast<std::size_t>(f.height);
        std::vector<std::uint8_t> idx = gif_detail::lzw_decode(data, f.min_code_size, count, f);
        if (f.interlaced) {  // rows stored in passes 0/8, 4/8, 2/4, 1/2
            std::vector<std::uint8_t> rows(idx.size());
            std::size_t src = 0;
            const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
            for (int pass = 0; pass < 4; ++pass)
                for (int y = start[pass]; y < f.height; y += step[pass], ++src)
                    for (int x = 0; x < f.width; ++x)
                        rows[static_cast<std::size_t>(y) * f.width + x] = idx[src * f.width + x];
            idx.swap(rows);
        }
        f.index = idx;
        f.rgb.resize(count * 3);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t e = static_cast<std::size_t>(idx[i]) * 3;
            if (e + 2 >= table.size()) throw std::runtime_error("GifRead: index beyond the colour table");
            for (int c = 0; c < 3; ++c) f.rgb[i * 3 + c] = table[e + c];
        }
        g.frames.push_back(std::move(f));
    }
    return g;
}

}  // namespace gdg::test
