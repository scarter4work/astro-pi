#include "Gif.h"

#include "AtomicFile.h"
#include "Parallel.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace gdg {

namespace {

constexpr int kPaletteSize = 256;
constexpr int kMinCodeSize = 8;
constexpr int kClear = 1 << kMinCodeSize;  // 256
constexpr int kEnd = kClear + 1;           // 257
constexpr int kFirstFree = kClear + 2;     // 258
constexpr int kMaxCodes = 4096;
constexpr int kMaxWidth = 12;
constexpr int kCellBits = 5;  // 32 cells per channel: 8 levels each
constexpr int kCells = 1 << (3 * kCellBits);

using Colour = std::array<int, 3>;

/// 24-bit RGB key of the pixel at p.
std::uint32_t key_at(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
}
int channel_of(std::uint32_t key, int c) { return static_cast<int>((key >> (16 - 8 * c)) & 0xFF); }

struct ColourCount {
    std::uint32_t key;
    std::uint64_t count;
};

/// A median-cut box: colours[begin, end) of the distinct-colour list.
struct Box {
    std::size_t begin = 0, end = 0;
    std::uint64_t count = 0;
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};

    int widest_channel() const {  // lowest channel on ties
        int best = 0;
        for (int c = 1; c < 3; ++c)
            if (hi[c] - lo[c] > hi[best] - lo[best]) best = c;
        return best;
    }
    int range() const { return hi[widest_channel()] - lo[widest_channel()]; }
};

Box make_box(const std::vector<ColourCount>& cs, std::size_t begin, std::size_t end) {
    Box b;
    b.begin = begin;
    b.end = end;
    for (std::size_t i = begin; i < end; ++i) {
        b.count += cs[i].count;
        for (int c = 0; c < 3; ++c) {
            const int v = channel_of(cs[i].key, c);
            b.lo[c] = std::min(b.lo[c], v);
            b.hi[c] = std::max(b.hi[c], v);
        }
    }
    return b;
}

/// The global palette by median cut (Gif.h): at most 256 entries, in box order.
std::vector<Colour> median_cut(const std::vector<std::vector<std::uint8_t>>& frames, std::size_t pixels_per_frame) {
    const std::size_t total = frames.size() * pixels_per_frame;
    const std::size_t step =
        total <= GIF_PALETTE_MAX_SAMPLES ? 1 : (total + GIF_PALETTE_MAX_SAMPLES - 1) / GIF_PALETTE_MAX_SAMPLES;
    std::vector<std::uint32_t> keys;
    keys.reserve(total / step + 1);
    for (std::size_t i = 0; i < total; i += step)
        keys.push_back(key_at(frames[i / pixels_per_frame].data() + (i % pixels_per_frame) * 3));
    std::sort(keys.begin(), keys.end());
    std::vector<ColourCount> cs;
    for (std::size_t i = 0; i < keys.size();) {
        std::size_t j = i;
        while (j < keys.size() && keys[j] == keys[i]) ++j;
        cs.push_back({keys[i], static_cast<std::uint64_t>(j - i)});
        i = j;
    }

    std::vector<Box> boxes{make_box(cs, 0, cs.size())};
    while (boxes.size() < static_cast<std::size_t>(kPaletteSize)) {
        // The splittable box (> 1 distinct colour) with the widest channel range, lowest index on ties.
        int pick = -1, best = -1;
        for (std::size_t i = 0; i < boxes.size(); ++i)
            if (boxes[i].end - boxes[i].begin > 1 && boxes[i].range() > best) {
                best = boxes[i].range();
                pick = static_cast<int>(i);
            }
        if (pick < 0) break;  // every box holds one distinct colour
        const Box b = boxes[static_cast<std::size_t>(pick)];
        const int ch = b.widest_channel();
        std::sort(cs.begin() + static_cast<std::ptrdiff_t>(b.begin), cs.begin() + static_cast<std::ptrdiff_t>(b.end),
                  [ch](const ColourCount& a, const ColourCount& c) {
                      const int va = channel_of(a.key, ch), vc = channel_of(c.key, ch);
                      return va != vc ? va < vc : a.key < c.key;
                  });
        // Pixel-weighted median: the first colour at which half the box's pixels are reached
        // closes the lower half; both halves keep at least one colour.
        std::uint64_t cum = 0;
        std::size_t split = b.begin + 1;
        for (std::size_t i = b.begin; i < b.end; ++i) {
            cum += cs[i].count;
            if (2 * cum >= b.count) {
                split = i + 1;
                break;
            }
        }
        split = std::clamp(split, b.begin + 1, b.end - 1);
        boxes[static_cast<std::size_t>(pick)] = make_box(cs, b.begin, split);
        boxes.push_back(make_box(cs, split, b.end));
    }

    std::vector<Colour> palette;
    palette.reserve(boxes.size());
    for (const Box& b : boxes) {
        std::uint64_t sum[3] = {0, 0, 0};
        for (std::size_t i = b.begin; i < b.end; ++i)
            for (int c = 0; c < 3; ++c) sum[c] += static_cast<std::uint64_t>(channel_of(cs[i].key, c)) * cs[i].count;
        Colour col{};
        for (int c = 0; c < 3; ++c) col[static_cast<std::size_t>(c)] = static_cast<int>((sum[c] + b.count / 2) / b.count);
        palette.push_back(col);
    }
    return palette;
}

int dist2(const Colour& p, int r, int g, int b) {
    const int dr = p[0] - r, dg = p[1] - g, db = p[2] - b;
    return dr * dr + dg * dg + db * db;
}

/// Per 32^3 cell, the palette entries (ascending) that can be nearest to some colour in the cell:
/// those whose smallest distance to the cell is within the smallest farthest distance of any
/// entry. The exact search then runs over them only, so results equal a full search.
std::vector<std::vector<std::uint8_t>> cell_candidates(const std::vector<Colour>& palette) {
    std::vector<std::vector<std::uint8_t>> cells(static_cast<std::size_t>(kCells));
    constexpr int kSpan = 256 >> kCellBits;  // 8 levels per cell
    gdg::parallel_for_dynamic(kCells, 256, [&](int cell, int) {
        const int lo[3] = {(cell >> (2 * kCellBits)) * kSpan, ((cell >> kCellBits) & ((1 << kCellBits) - 1)) * kSpan,
                           (cell & ((1 << kCellBits) - 1)) * kSpan};
        std::vector<int> dmin(palette.size());
        int minmax = INT_MAX;
        for (std::size_t p = 0; p < palette.size(); ++p) {
            int near = 0, far = 0;
            for (int c = 0; c < 3; ++c) {
                const int v = palette[p][static_cast<std::size_t>(c)], a = lo[c], b = lo[c] + kSpan - 1;
                const int dn = v < a ? a - v : (v > b ? v - b : 0);
                const int df = std::max(std::abs(v - a), std::abs(v - b));
                near += dn * dn;
                far += df * df;
            }
            dmin[p] = near;
            minmax = std::min(minmax, far);
        }
        std::vector<std::uint8_t>& list = cells[static_cast<std::size_t>(cell)];
        for (std::size_t p = 0; p < palette.size(); ++p)
            if (dmin[p] <= minmax) list.push_back(static_cast<std::uint8_t>(p));
    });
    return cells;
}

/// GIF bit packer: codes LSB-first into a byte stream.
struct BitWriter {
    std::vector<std::uint8_t>& out;
    std::uint32_t acc = 0;
    int bits = 0;
    void put(int code, int width) {
        acc |= static_cast<std::uint32_t>(code) << bits;
        bits += width;
        while (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>(acc & 0xFF));
            acc >>= 8;
            bits -= 8;
        }
    }
    void flush() {
        if (bits > 0) out.push_back(static_cast<std::uint8_t>(acc & 0xFF));
        acc = 0;
        bits = 0;
    }
};

/// The LZW string table: (prefix code, next index) -> code, open addressing.
class LzwTable {
public:
    LzwTable() : keys_(kSlots, -1), codes_(kSlots, 0) {}
    void clear() { std::fill(keys_.begin(), keys_.end(), -1); }
    int find(int prefix, int k) const {
        const int key = (prefix << 8) | k;
        for (std::size_t s = slot(key);; s = (s + 1) & (kSlots - 1)) {
            if (keys_[s] == key) return codes_[s];
            if (keys_[s] < 0) return -1;
        }
    }
    void insert(int prefix, int k, int code) {
        const int key = (prefix << 8) | k;
        std::size_t s = slot(key);
        while (keys_[s] >= 0) s = (s + 1) & (kSlots - 1);
        keys_[s] = key;
        codes_[s] = static_cast<std::uint16_t>(code);
    }

private:
    static constexpr std::size_t kSlots = 16384;  // >= 4 x the 3838 entries a table holds
    static std::size_t slot(int key) {
        return (static_cast<std::uint32_t>(key) * 2654435761u) >> (32 - 14);
    }
    std::vector<int> keys_;
    std::vector<std::uint16_t> codes_;
};

/// One image's LZW code stream (before sub-blocking): a leading clear, the codes, the end code.
/// After each code the width grows when the next code to assign reaches 1 << width (at most 12),
/// which is where a decoder, one table entry behind, grows its own; a full table (4096 codes)
/// emits a clear and starts over.
std::vector<std::uint8_t> lzw_encode(const std::vector<std::uint8_t>& idx) {
    std::vector<std::uint8_t> out;
    out.reserve(idx.size() / 2 + 16);
    BitWriter bw{out};
    LzwTable table;
    int width = kMinCodeSize + 1, next = kFirstFree;
    bw.put(kClear, width);
    int cur = idx[0];
    auto emit = [&](int code) {
        bw.put(code, width);
        if (next >= (1 << width) && width < kMaxWidth) ++width;
    };
    for (std::size_t i = 1; i < idx.size(); ++i) {
        const int k = idx[i];
        const int found = table.find(cur, k);
        if (found >= 0) {
            cur = found;
            continue;
        }
        emit(cur);
        table.insert(cur, k, next++);
        if (next == kMaxCodes) {
            bw.put(kClear, width);
            table.clear();
            width = kMinCodeSize + 1;
            next = kFirstFree;
        }
        cur = k;
    }
    emit(cur);
    bw.put(kEnd, width);
    bw.flush();
    return out;
}

void put16(std::vector<std::uint8_t>& out, int v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void check_args(const std::vector<std::vector<std::uint8_t>>& frames, int width, int height, int delay_cs, int loops) {
    auto bad = [](const std::string& what) { throw std::invalid_argument("GIF: " + what); };
    if (frames.empty()) bad("no frames");
    if (width < 1 || width > 65535 || height < 1 || height > 65535)
        bad("width and height must be in [1, 65535] (got " + std::to_string(width) + "x" + std::to_string(height) + ")");
    if (delay_cs < 0 || delay_cs > 65535) bad("delay_cs must be in [0, 65535] (got " + std::to_string(delay_cs) + ")");
    if (loops < 0 || loops > 65535) bad("loops must be in [0, 65535] (got " + std::to_string(loops) + ")");
    const std::size_t want = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3;
    for (std::size_t k = 0; k < frames.size(); ++k)
        if (frames[k].size() != want)
            bad("frame " + std::to_string(k) + " holds " + std::to_string(frames[k].size()) + " bytes, " +
                std::to_string(want) + " expected for " + std::to_string(width) + "x" + std::to_string(height) + " RGB");
}

}  // namespace

std::vector<std::uint8_t> encode_gif(const std::vector<std::vector<std::uint8_t>>& frames_rgb, int width, int height,
                                     int delay_cs, int loops) {
    check_args(frames_rgb, width, height, delay_cs, loops);
    const std::size_t ppf = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::vector<Colour> palette = median_cut(frames_rgb, ppf);
    const std::vector<std::vector<std::uint8_t>> cells = cell_candidates(palette);

    // Map to palette indices: each frame's rows in parallel, each pixel's exact nearest entry.
    std::vector<std::vector<std::uint8_t>> index(frames_rgb.size(), std::vector<std::uint8_t>(ppf));
    for (std::size_t f = 0; f < frames_rgb.size(); ++f) {
        const std::uint8_t* src = frames_rgb[f].data();
        std::uint8_t* dst = index[f].data();
        gdg::parallel_for_dynamic(height, std::max(1, 16384 / width), [&](int y, int) {
            const std::size_t end = static_cast<std::size_t>(y + 1) * static_cast<std::size_t>(width);
            for (std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(width); i < end; ++i) {
                const int r = src[i * 3], g = src[i * 3 + 1], b = src[i * 3 + 2];
                const int cell = ((r >> 3) << (2 * kCellBits)) | ((g >> 3) << kCellBits) | (b >> 3);
                int best = INT_MAX;
                std::uint8_t pick = 0;
                for (const std::uint8_t p : cells[static_cast<std::size_t>(cell)]) {
                    const int d = dist2(palette[p], r, g, b);
                    if (d < best) {  // ascending candidates: the lowest index wins ties
                        best = d;
                        pick = p;
                    }
                }
                dst[i] = pick;
            }
        });
    }

    std::vector<std::vector<std::uint8_t>> lzw(frames_rgb.size());
    gdg::parallel_for_dynamic(static_cast<int>(frames_rgb.size()), 1, [&](int f, int) {
        lzw[static_cast<std::size_t>(f)] = lzw_encode(index[static_cast<std::size_t>(f)]);
    });

    std::vector<std::uint8_t> out;
    std::size_t reserve = 6 + 7 + 3 * kPaletteSize + 19 + 1;
    for (const auto& l : lzw) reserve += 8 + 10 + 1 + l.size() + l.size() / 255 + 2;
    out.reserve(reserve);
    const char* sig = "GIF89a";
    out.insert(out.end(), sig, sig + 6);
    // Logical screen descriptor: global table, colour resolution 8 bits, unsorted, 2^(7+1) entries.
    put16(out, width);
    put16(out, height);
    out.push_back(0xF7);
    out.push_back(0);  // background colour index
    out.push_back(0);  // pixel aspect ratio: none given
    for (int p = 0; p < kPaletteSize; ++p)
        for (int c = 0; c < 3; ++c)
            out.push_back(static_cast<std::uint8_t>(
                p < static_cast<int>(palette.size()) ? palette[static_cast<std::size_t>(p)][static_cast<std::size_t>(c)] : 0));
    // NETSCAPE2.0 looping extension.
    const char* app = "NETSCAPE2.0";
    out.insert(out.end(), {0x21, 0xFF, 0x0B});
    out.insert(out.end(), app, app + 11);
    out.insert(out.end(), {0x03, 0x01});
    put16(out, loops);
    out.push_back(0x00);
    for (const auto& data : lzw) {
        // Graphic control extension: disposal 1 (do not dispose), no transparency, the delay.
        out.insert(out.end(), {0x21, 0xF9, 0x04, 0x04});
        put16(out, delay_cs);
        out.insert(out.end(), {0x00, 0x00});
        // Image descriptor: the whole screen, no local table, not interlaced.
        out.push_back(0x2C);
        put16(out, 0);
        put16(out, 0);
        put16(out, width);
        put16(out, height);
        out.push_back(0x00);
        out.push_back(static_cast<std::uint8_t>(kMinCodeSize));
        for (std::size_t p = 0; p < data.size(); p += 255) {
            const std::size_t n = std::min<std::size_t>(255, data.size() - p);
            out.push_back(static_cast<std::uint8_t>(n));
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(p),
                       data.begin() + static_cast<std::ptrdiff_t>(p + n));
        }
        out.push_back(0x00);
    }
    out.push_back(0x3B);
    return out;
}

void write_gif(const std::string& path, const std::vector<std::vector<std::uint8_t>>& frames_rgb, int width, int height,
               int delay_cs, int loops) {
    write_file_atomic(path, encode_gif(frames_rgb, width, height, delay_cs, loops));
}

}  // namespace gdg
