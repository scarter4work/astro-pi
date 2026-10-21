#include "AviFile.h"

#include <algorithm>
#include <cstring>

namespace planetary::ser {

using avi_detail::StreamIds;

namespace {
constexpr int32_t kMaxDimension = 65536;
constexpr int64_t kMaxHeaderListBytes = 16 << 20;  // hdrl is ~33 KB on real captures
constexpr uint32_t kSizeMask = 0x7FFFFFFFu;        // ix## dwSize bit 31 = delta-frame flag
constexpr int64_t kMinAvixSize = 4 + 12;           // 'AVIX' + an empty LIST movi
constexpr int64_t kGapReadWhole = 1 << 20;         // gaps up to this size are read in one go
constexpr uint32_t kPalChangesFlag = 0x00010000u;  // strh AVISF_VIDEO_PALCHANGES

uint16_t U16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

uint32_t U32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

uint64_t U64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool Is(const unsigned char* p, const char* id) { return std::memcmp(p, id, 4) == 0; }

std::string Num(int64_t v) { return std::to_string(v); }

// A fourcc as text when printable, else as hex (e.g. BI_RLE8 = 1 -> "0x00000001").
std::string FourccName(const unsigned char* p) {
    bool printable = true;
    for (int i = 0; i < 4; ++i) printable = printable && p[i] >= 0x20 && p[i] < 0x7F;
    if (printable) return std::string(reinterpret_cast<const char*>(p), 4);
    static const char* hex = "0123456789ABCDEF";
    std::string s = "0x";
    for (int i = 3; i >= 0; --i) { s += hex[p[i] >> 4]; s += hex[p[i] & 15]; }
    return s;
}

// The 8-byte chunk header `h` is this stream's video chunk of the expected size.
bool IsVideoHeader(const unsigned char* h, const StreamIds& ids, int64_t chunkBytes) {
    return ids.IsFrame(h) && U32(h + 4) == static_cast<uint64_t>(chunkBytes);
}

// One RIFF/LIST child: header at `pos`, `size` data bytes from pos + 8.
struct Chunk {
    unsigned char id[4] = {};
    unsigned char listType[4] = {};  // LIST/RIFF only
    int64_t pos = 0;
    int64_t size = 0;
    int64_t DataEnd() const { return pos + 8 + size; }
    int64_t Next() const { return DataEnd() + (size & 1); }
};

// Positioned reads over the untrusted file; every read is bounds-checked first.
class Io {
public:
    Io(std::ifstream& in, int64_t fileSize) : in_(in), fileSize_(fileSize) {}

    bool ReadAt(int64_t pos, void* dst, int64_t n) const {
        if (pos < 0 || n < 0 || pos > fileSize_ || n > fileSize_ - pos) return false;
        in_.clear();
        in_.seekg(static_cast<std::streamoff>(pos));
        in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
        return in_ && in_.gcount() == static_cast<std::streamsize>(n);
    }

    // Header (and list type) of the chunk at pos, when its 8-byte header fits before `end`.
    bool ChunkAt(int64_t pos, int64_t end, Chunk& c) const {
        unsigned char h[12];
        if (pos + 8 > end || !ReadAt(pos, h, 8)) return false;
        std::memcpy(c.id, h, 4);
        c.pos = pos;
        c.size = U32(h + 4);
        if ((Is(h, "LIST") || Is(h, "RIFF")) && c.size >= 4) {
            if (pos + 12 > end || !ReadAt(pos + 8, c.listType, 4)) return false;
        } else {
            std::memset(c.listType, 0, 4);
        }
        return true;
    }

    int64_t FileSize() const { return fileSize_; }

private:
    std::ifstream& in_;
    int64_t fileSize_;
};

[[noreturn]] void ThrowPaletteChange(const StreamIds& ids, int64_t pos) {
    throw SerError(std::string("palette-change chunk (") + ids.digits[0] + ids.digits[1] + "pc) at offset " +
                   Num(pos) + "; changing palettes are not supported");
}

// Collects usable frame offsets in file order. Entry numbers count every video entry,
// zero-length ones included. The first invalid entry ends the table. Index entries are
// checked arithmetically only (inside the file, padded frame size, ascending, no overlap):
// reading every chunk header would cost one NAS disk seek per frame (18 s for 3618 frames,
// measured). SpotCheck reads three headers; ReadFrame verifies each frame's own header.
class FrameTable {
public:
    FrameTable(int64_t fileSize, int64_t chunkBytes)
        : fileSize_(fileSize), chunkBytes_(chunkBytes), maxEntries_(fileSize / 8) {}

    // An index entry: `dataPos` is where the chunk data starts. False = stop.
    bool AddIndexed(int64_t dataPos, int64_t size) {
        if (entries_ >= maxEntries_) return Broken();  // more entries than the file can hold
        const int64_t entry = entries_++;
        if (size == 0) return Drop(entry);
        const bool valid = size == chunkBytes_ && dataPos >= 8 && dataPos >= nextFree_ &&
                           dataPos <= fileSize_ - size;
        if (!valid) return Invalid(entry);
        Push(dataPos, entry);
        return true;
    }

    // A frame chunk met while scanning; `complete` = its data lies inside its list. False = stop.
    bool AddScanned(const Chunk& c, bool complete) {
        const int64_t entry = entries_++;
        if (complete && c.size == 0) return Drop(entry);
        if (!complete || c.size != chunkBytes_)
            return Stop("video chunk " + Num(entry) + " is truncated or malformed; using the first " +
                        Num(Count()) + " frames");
        Push(c.pos + 8, entry);
        return true;
    }

    // An index structure (ix## chunk) is unreadable: frames from here on are lost.
    bool Broken() { return Invalid(entries_); }

    // Reads the chunk headers of the first, middle and last usable frames; the first one that
    // is not this stream's video chunk of the expected size ends the table there.
    void SpotCheck(const Io& io, const StreamIds& ids) {
        const int64_t n = Count();
        if (n == 0) return;
        for (int64_t k : {int64_t(0), n / 2, n - 1}) {
            unsigned char h[8];
            if (io.ReadAt(offsets_[k] - 8, h, 8) && IsVideoHeader(h, ids, chunkBytes_)) continue;
            const int64_t entry = entryOf_[k];
            offsets_.resize(static_cast<size_t>(k));
            entryOf_.resize(static_cast<size_t>(k));
            Invalid(entry);
            return;
        }
    }

    bool Stopped() const { return stopped_; }
    int64_t Count() const { return static_cast<int64_t>(offsets_.size()); }
    int64_t Dropped() const { return dropped_; }
    const std::string& StopWarning() const { return stopWarning_; }
    const std::vector<int64_t>& Offsets() const { return offsets_; }
    std::vector<int64_t> TakeOffsets() { return std::move(offsets_); }
    std::vector<int64_t> TakeEntries() { return std::move(entryOf_); }

private:
    // A zero-length (dropped) frame; a run longer than the cap ends the table.
    bool Drop(int64_t entry) {
        if (++run_ > AviFile::kMaxDroppedRun)
            return Stop("more than " + Num(AviFile::kMaxDroppedRun) + " consecutive zero-length frames at entry " +
                        Num(entry) + "; using the first " + Num(Count()) + " frames");
        ++dropped_;
        return true;
    }
    bool Invalid(int64_t entry) {
        return Stop("index entry " + Num(entry) + " is invalid; using the first " + Num(Count()) + " frames");
    }
    void Push(int64_t dataPos, int64_t entry) {
        offsets_.push_back(dataPos);
        entryOf_.push_back(entry);
        run_ = 0;
        nextFree_ = dataPos + chunkBytes_ + 8;  // the next chunk header follows: no overlap, table bounded by the file
    }
    bool Stop(std::string warning) {
        stopped_ = true;
        stopWarning_ = std::move(warning);
        return false;
    }

    int64_t fileSize_, chunkBytes_, maxEntries_;
    int64_t entries_ = 0, dropped_ = 0, run_ = 0, nextFree_ = 0;
    bool stopped_ = false;
    std::string stopWarning_;
    std::vector<int64_t> offsets_;
    std::vector<int64_t> entryOf_;  // index entry number of each usable frame
};

// The chunks inside a header-list buffer (hdrl/strl), bounds-checked against the buffer.
template <typename Fn>
void ForEachInBuffer(const unsigned char* p, int64_t n, Fn fn) {
    int64_t pos = 0;
    while (pos + 8 <= n) {
        const int64_t size = U32(p + pos + 4);
        if (size > n - pos - 8) throw SerError("AVI header chunk '" + FourccName(p + pos) + "' overruns its list");
        fn(p + pos, p + pos + 8, size);
        pos += 8 + size + (size & 1);
    }
}

struct VideoStream {
    bool found = false;
    StreamIds ids;
    const unsigned char* strh = nullptr; int64_t strhSize = 0;
    const unsigned char* strf = nullptr; int64_t strfSize = 0;
    const unsigned char* indx = nullptr; int64_t indxSize = 0;
};

// OpenDML super index -> ix## standard indexes. Returns false (with `why`) when the super
// index itself is malformed, so the caller falls back to idx1 or a scan.
bool BuildFromSuperIndex(const VideoStream& vs, const Io& io, FrameTable& table, std::string& why) {
    const unsigned char* d = vs.indx;
    if (vs.indxSize < 24) { why = "shorter than its 24-byte header"; return false; }
    if (U16(d) != 4 || d[2] != 0 || d[3] != 0) {
        why = "not an index of indexes";
        return false;
    }
    const int64_t n = U32(d + 4);
    if (n == 0) { why = "no entries"; return false; }
    if (n > (vs.indxSize - 24) / 16) { why = Num(n) + " entries do not fit in the chunk"; return false; }
    if (!vs.ids.IsFrame(d + 8)) { why = "indexes chunk '" + FourccName(d + 8) + "', not the video stream"; return false; }

    // ix## chunks must be distinct, ascending and non-overlapping, so the total index work
    // is bounded by the file size however the super index repeats or overstates them.
    uint64_t nextIx = 0;
    for (int64_t s = 0; s < n && !table.Stopped(); ++s) {
        const unsigned char* e = d + 24 + 16 * s;
        const uint64_t ixPos = U64(e);
        unsigned char h[32];
        if (ixPos < nextIx || ixPos > static_cast<uint64_t>(io.FileSize()) ||
            !io.ReadAt(static_cast<int64_t>(ixPos), h, 32) || !vs.ids.IsIndex(h)) {
            table.Broken();
            break;
        }
        const int64_t ixSize = U32(h + 4);
        const int64_t count = U32(h + 12);
        const unsigned char* ix = h + 8;
        if (U16(ix) != 2 || ix[2] != 0 || ix[3] != 1 || !vs.ids.IsFrame(ix + 8) ||
            count > (ixSize - 24) / 8 || count > (io.FileSize() - static_cast<int64_t>(ixPos) - 32) / 8) {
            table.Broken();
            break;
        }
        nextIx = ixPos + 8 + static_cast<uint64_t>(ixSize);
        const uint64_t base = U64(ix + 12);
        std::vector<unsigned char> entries(static_cast<size_t>(8 * count));
        if (base > static_cast<uint64_t>(io.FileSize()) ||
            !io.ReadAt(static_cast<int64_t>(ixPos) + 32, entries.data(), 8 * count)) {
            table.Broken();
            break;
        }
        for (int64_t i = 0; i < count; ++i) {
            const uint64_t dataPos = base + U32(&entries[8 * i]);
            const int64_t size = U32(&entries[8 * i + 4]) & kSizeMask;
            const int64_t pos = dataPos > static_cast<uint64_t>(io.FileSize()) ? -1 : static_cast<int64_t>(dataPos);
            if (!table.AddIndexed(pos, size)) break;
        }
    }
    return true;
}

// idx1: 16-byte entries {ckid, flags, offset, size}; offset points at the chunk HEADER,
// relative to the movi list's fourcc in most writers and absolute in some (ASIAIR) --
// decided from the first video entry.
void BuildFromIdx1(const std::vector<unsigned char>& idx1, int64_t moviFourcc, const VideoStream& vs,
                   const Io& io, FrameTable& table) {
    const int64_t n = static_cast<int64_t>(idx1.size() / 16);
    int64_t base = -1;
    for (int64_t i = 0; i < n && !table.Stopped(); ++i) {
        const unsigned char* e = &idx1[16 * i];
        if (!vs.ids.IsFrame(e)) continue;
        const int64_t off = U32(e + 8), size = U32(e + 12);
        if (base < 0) {
            for (int64_t candidate : {moviFourcc, int64_t(0)}) {
                unsigned char h[4];
                if (io.ReadAt(candidate + off, h, 4) && std::memcmp(h, e, 4) == 0) { base = candidate; break; }
            }
            if (base < 0) { table.Broken(); break; }  // the first video entry points at nothing
        }
        table.AddIndexed(base + off + 8, size);
    }
}

// Linear walk of the movi lists (descending into 'rec ' lists). Reads chunk headers only.
// Returns false once the table has stopped.
bool ScanList(const Io& io, int64_t start, int64_t end, const StreamIds& ids, FrameTable& table, int depth = 0) {
    Chunk c;
    for (int64_t pos = start; io.ChunkAt(pos, end, c); pos = c.Next()) {
        const bool inside = c.DataEnd() <= end;
        if (ids.IsFrame(c.id)) {
            if (!table.AddScanned(c, inside)) return false;
        } else if (ids.IsPaletteChange(c.id)) {
            ThrowPaletteChange(ids, c.pos);
        } else if (!inside) {
            return true;  // a damaged list: nothing further in it can be trusted
        } else if (Is(c.id, "LIST") && Is(c.listType, "rec ")) {
            if (depth + 1 > AviFile::kMaxRecDepth)
                throw SerError("AVI 'rec ' lists are nested " + Num(depth + 1) + " deep at offset " + Num(c.pos) +
                               " (at most " + Num(AviFile::kMaxRecDepth) + " supported)");
            if (!ScanList(io, c.pos + 12, c.DataEnd(), ids, table, depth + 1)) return false;
        }
    }
    return true;
}

// The bytes between two usable frames (JUNK, ix##, other streams, dropped frames, and at a
// segment boundary idx1 and the RIFF/LIST headers): throws on a palette-change chunk.
// Lists are stepped into, everything else skipped by its size. `filePos` = p's offset.
void CheckGapBytes(const unsigned char* p, int64_t n, int64_t filePos, const StreamIds& ids) {
    for (int64_t pos = 0; pos + 8 <= n;) {
        if (ids.IsPaletteChange(p + pos)) ThrowPaletteChange(ids, filePos + pos);
        const int64_t size = U32(p + pos + 4);
        pos += (Is(p + pos, "LIST") || Is(p + pos, "RIFF")) ? 12 : 8 + size + (size & 1);
    }
}

// CheckGapBytes over [start, end) of the file, the gap before usable frame `frame`: one read
// when the gap is small (the ASIAIR gap is a 504-byte JUNK), else a walk of its chunk headers
// (a segment boundary's idx1), one read each and at most AviFile::kMaxGapChunks of them.
void CheckGap(const Io& io, int64_t start, int64_t end, const StreamIds& ids, int64_t frame) {
    if (end - start <= kGapReadWhole) {
        std::vector<unsigned char> gap(static_cast<size_t>(std::max<int64_t>(end - start, 0)));
        if (io.ReadAt(start, gap.data(), static_cast<int64_t>(gap.size())))
            CheckGapBytes(gap.data(), static_cast<int64_t>(gap.size()), start, ids);
        return;
    }
    Chunk c;
    int64_t chunks = 0;
    for (int64_t pos = start; io.ChunkAt(pos, end, c);) {
        if (++chunks > AviFile::kMaxGapChunks)
            throw SerError("the gap before frame " + Num(frame) + " holds more than " +
                           Num(AviFile::kMaxGapChunks) + " chunks");
        if (ids.IsPaletteChange(c.id)) ThrowPaletteChange(ids, c.pos);
        pos = (Is(c.id, "LIST") || Is(c.id, "RIFF")) ? pos + 12 : c.Next();
    }
}

struct Segment {
    int64_t pos = 0;
    int64_t end = 0;  // end of its data, clamped to the file
};

// The LIST movi of a segment as [start of its children, end clamped to the segment); false if none.
bool FindMovi(const Io& io, const Segment& seg, int64_t& start, int64_t& end) {
    Chunk c;
    for (int64_t pos = seg.pos + 12; io.ChunkAt(pos, seg.end, c); pos = c.Next()) {
        if (Is(c.id, "LIST") && Is(c.listType, "movi")) {
            start = c.pos + 12;
            end = std::min(c.DataEnd(), seg.end);
            return true;
        }
    }
    return false;
}

}  // namespace

AviFile AviFile::Open(const std::string& path) {
    AviFile f;
    f.path_ = path;
    f.stream_.open(path, std::ios::binary);
    if (!f.stream_) throw SerError("cannot open AVI file: " + path);
    f.stream_.seekg(0, std::ios::end);
    const std::streamoff endPos = f.stream_.tellg();
    if (endPos < 0) throw SerError("cannot determine size of AVI file: " + path);
    f.fileSize_ = static_cast<int64_t>(endPos);
    const Io io(f.stream_, f.fileSize_);

    // ---- RIFF segments: 'AVI ' then any number of 'AVIX'; bytes after the last are ignored.
    // Each AVIX must hold at least an empty LIST movi, and there are at most kMaxSegments
    // (real ASIAIR segments are ~1 GB), so a crafted file cannot make this walk long.
    Chunk riff;
    if (!io.ChunkAt(0, f.fileSize_, riff) || !Is(riff.id, "RIFF") || !Is(riff.listType, "AVI "))
        throw SerError("not an AVI file: it does not start with RIFF....AVI ");
    std::vector<Segment> segments{{0, std::min(riff.DataEnd(), f.fileSize_)}};
    for (int64_t next = riff.Next(); next + 12 <= f.fileSize_;) {
        Chunk c;
        if (!io.ChunkAt(next, f.fileSize_, c)) break;
        if (!Is(c.id, "RIFF")) {
            // ASIAIR writes the size of each AVIX segment after the first 8 bytes too large,
            // so its successor starts 8 bytes before the declared end (measured on the NAS).
            Chunk q;
            if (next - 8 > segments.back().pos && io.ChunkAt(next - 8, f.fileSize_, q) &&
                Is(q.id, "RIFF") && Is(q.listType, "AVIX")) {
                c = q;
            } else {
                break;
            }
        }
        if (!Is(c.listType, "AVIX"))
            throw SerError("unexpected RIFF form '" + FourccName(c.listType) + "' at offset " + Num(c.pos) +
                           " (only AVIX may follow the AVI segment)");
        if (c.size < kMinAvixSize)
            throw SerError("AVIX segment at offset " + Num(c.pos) + " is too small (" + Num(c.size) + " bytes)");
        if (static_cast<int64_t>(segments.size()) >= AviFile::kMaxSegments)
            throw SerError("AVI has more than " + Num(AviFile::kMaxSegments) + " RIFF segments");
        segments.push_back({c.pos, std::min(c.DataEnd(), f.fileSize_)});
        next = c.Next();
    }

    // ---- top-level chunks of the first segment
    Chunk hdrl, movi, idx1Chunk;
    bool haveHdrl = false, haveMovi = false, haveIdx1 = false;
    {
        Chunk c;
        for (int64_t pos = 12; io.ChunkAt(pos, segments[0].end, c); pos = c.Next()) {
            if (Is(c.id, "LIST") && Is(c.listType, "hdrl") && !haveHdrl) { hdrl = c; haveHdrl = true; }
            else if (Is(c.id, "LIST") && Is(c.listType, "movi") && !haveMovi) { movi = c; haveMovi = true; }
            else if (Is(c.id, "idx1") && haveMovi && !haveIdx1 && c.DataEnd() <= segments[0].end) {
                idx1Chunk = c;
                haveIdx1 = true;
            }
        }
    }
    if (!haveHdrl) throw SerError("AVI has no hdrl header list");
    if (!haveMovi) throw SerError("AVI has no movi list");
    if (hdrl.size > kMaxHeaderListBytes)
        throw SerError("AVI hdrl header list is implausibly large (" + Num(hdrl.size) + " bytes)");
    if (hdrl.DataEnd() > f.fileSize_) throw SerError("AVI hdrl header list is cut short");
    std::vector<unsigned char> hdr(static_cast<size_t>(hdrl.size - 4));
    if (!io.ReadAt(hdrl.pos + 12, hdr.data(), hdrl.size - 4)) throw SerError("failed to read the AVI header");

    // ---- avih + the first video stream
    uint32_t usPerFrame = 0;
    VideoStream vs;
    int streamNo = 0;
    ForEachInBuffer(hdr.data(), static_cast<int64_t>(hdr.size()),
                    [&](const unsigned char* id, const unsigned char* d, int64_t n) {
        if (Is(id, "avih") && n >= 4) usPerFrame = U32(d);
        if (!Is(id, "LIST") || n < 4 || !Is(d, "strl")) return;
        VideoStream s;
        ForEachInBuffer(d + 4, n - 4, [&](const unsigned char* cid, const unsigned char* cd, int64_t cn) {
            if (Is(cid, "strh") && !s.strh) { s.strh = cd; s.strhSize = cn; }
            else if (Is(cid, "strf") && !s.strf) { s.strf = cd; s.strfSize = cn; }
            else if (Is(cid, "indx") && !s.indx) { s.indx = cd; s.indxSize = cn; }
        });
        if (!vs.found && s.strh && s.strhSize >= 4 && Is(s.strh, "vids") && streamNo < 100) {
            vs = s;
            vs.found = true;
            vs.ids.digits[0] = static_cast<char>('0' + streamNo / 10);
            vs.ids.digits[1] = static_cast<char>('0' + streamNo % 10);
        }
        ++streamNo;
    });
    if (!vs.found) throw SerError("AVI has no video stream");
    if (vs.strhSize < 28) throw SerError("AVI video stream header (strh) is too short");
    if (!vs.strf || vs.strfSize < 40) throw SerError("AVI video stream has no BITMAPINFOHEADER (strf)");

    // ---- format: 8-bit grey only
    const unsigned char* bi = vs.strf;
    const int64_t biSize = U32(bi);
    const int64_t width = static_cast<int32_t>(U32(bi + 4));
    const int64_t biHeight = static_cast<int32_t>(U32(bi + 8));
    const int bitCount = U16(bi + 14);
    const unsigned char* compression = bi + 16;
    const bool biRgb = U32(compression) == 0;
    const std::string codec = biRgb ? "BI_RGB" : FourccName(compression);
    const bool grey = Is(compression, "Y800") || Is(compression, "GREY") || Is(compression, "Y8  ");
    if (!(biRgb || grey) || bitCount != 8)
        throw SerError("not supported: " + codec + " " + Num(bitCount) + "-bit video");
    if (biSize < 40 || biSize > vs.strfSize) throw SerError("AVI BITMAPINFOHEADER size " + Num(biSize) + " is invalid");
    if (biRgb) {
        const int64_t entries = U32(bi + 32) == 0 ? 256 : U32(bi + 32);
        if (entries != 256)
            throw SerError("not supported: palette has " + Num(entries) +
                           " entries (needs the 256-entry identity grey ramp)");
        if (biSize + 4 * entries > vs.strfSize) throw SerError("AVI palette is cut short");
        std::string differing;
        int count = 0;
        for (int i = 0; i < 256 && count < 3; ++i) {
            const unsigned char* q = bi + biSize + 4 * i;  // RGBQUAD: blue, green, red, reserved
            if (q[0] == i && q[1] == i && q[2] == i) continue;
            differing += (count++ ? ", " : "") + Num(i) + "=(" + Num(q[2]) + "," + Num(q[1]) + "," + Num(q[0]) + ")";
        }
        if (count)
            throw SerError("not supported: palette is not the identity grey ramp (first differing entries: " +
                           differing + ")");
    }
    const int64_t height = biHeight < 0 ? -biHeight : biHeight;
    if (width <= 0 || height <= 0)
        throw SerError("invalid image size " + Num(width) + "x" + Num(biHeight));
    if (width > kMaxDimension || height > kMaxDimension)
        throw SerError("image size " + Num(width) + "x" + Num(height) + " exceeds the supported maximum of " +
                       Num(kMaxDimension));
    // BITMAPINFOHEADER: an RGB DIB with biHeight > 0 is bottom-up; YUV formats (Y800 and kin)
    // are always top-down whatever the sign.
    f.bottomUp_ = biRgb && biHeight > 0;
    f.stride_ = (width + 3) & ~int64_t(3);
    const int64_t chunkBytes = f.stride_ * height;

    FileInfo& info = f.info_;
    Header& h = info.header;
    h.width = static_cast<int32_t>(width);
    h.height = static_cast<int32_t>(height);
    h.depth = 8;
    info.normDepth = 8;
    info.colorId = ColorId::Mono;
    info.planes = 1;
    info.bytesPerSample = 1;
    info.frameBytes = width * height;
    info.hasTimestamps = false;
    info.container = (vs.indx || segments.size() > 1) ? Container::AviOpenDml : Container::Avi;
    AviDetails details;
    details.bayerSource = "none";
    const uint32_t scale = U32(vs.strh + 20), rate = U32(vs.strh + 24);
    if (rate > 0 && scale > 0) details.frameRate = static_cast<double>(rate) / scale;
    else if (usPerFrame > 0) details.frameRate = 1e6 / usPerFrame;

    // ---- idx1 (also checked for palette changes whichever index is used)
    std::vector<unsigned char> idx1;
    if (haveIdx1) {
        idx1.resize(static_cast<size_t>(idx1Chunk.size / 16 * 16));
        if (!io.ReadAt(idx1Chunk.pos + 8, idx1.data(), static_cast<int64_t>(idx1.size())))
            throw SerError("failed to read the AVI idx1 index");
        for (size_t i = 0; i + 16 <= idx1.size(); i += 16)
            if (vs.ids.IsPaletteChange(&idx1[i])) ThrowPaletteChange(vs.ids, int64_t(U32(&idx1[i + 8])));
    }

    // ---- frame table: indx -> ix##, else idx1, else a scan of the movi lists
    FrameTable table(f.fileSize_, chunkBytes);
    bool indexed = false;
    if (vs.indx) {
        std::string why;
        if (!BuildFromSuperIndex(vs, io, table, why)) {
            info.warnings.push_back("OpenDML indx is malformed (" + why + "); not used");
        } else {
            table.SpotCheck(io, vs.ids);
            if (table.Count() == 0) {
                const std::string reason = table.Stopped()       ? table.StopWarning()
                                           : table.Dropped() > 0 ? Num(table.Dropped()) + " zero-length entries"
                                                                 : std::string("OpenDML index has 0 entries");
                info.warnings.push_back("OpenDML indx gave no usable frames (" + reason + "); not used");
                table = FrameTable(f.fileSize_, chunkBytes);
            } else {
                details.indexKind = "OpenDML indx";
                indexed = true;
            }
        }
    }
    if (!indexed && haveIdx1 && segments.size() == 1) {
        BuildFromIdx1(idx1, movi.pos + 8, vs, io, table);
        table.SpotCheck(io, vs.ids);
        details.indexKind = "idx1";
        indexed = true;
    }
    if (indexed) {
        // Palette-change rule (Ruling 4): open walks every gap between frames only when strh
        // declares AVISF_VIDEO_PALCHANGES, because ASIAIR writes a JUNK chunk after every frame
        // and the full walk costs one NAS disk seek per frame (2440 reads, 14.2 s cold,
        // measured); otherwise open checks idx1 and the gap before frame 0, and ReadFrame the
        // gap immediately before the frame it decodes. A change in a gap before an unread
        // frame is the accepted limit.
        if (table.Count() > 0) CheckGap(io, movi.pos + 12, table.Offsets()[0] - 8, vs.ids, 0);
        if (U32(vs.strh + 8) & kPalChangesFlag) {
            const std::vector<int64_t>& o = table.Offsets();
            for (size_t k = 1; k < o.size(); ++k)
                CheckGap(io, o[k - 1] + chunkBytes + (chunkBytes & 1), o[k] - 8, vs.ids, static_cast<int64_t>(k));
            f.allGapsChecked_ = true;
        }
    } else {
        info.warnings.push_back(haveIdx1 ? "idx1 covers only the first RIFF segment; scanned the file for frames"
                                         : "no index; scanned the file for frames");
        details.indexKind = "scanned";
        for (const Segment& seg : segments) {
            int64_t start = 0, end = 0;
            if (FindMovi(io, seg, start, end) && !ScanList(io, start, end, vs.ids, table)) break;
        }
        f.allGapsChecked_ = true;  // the scan saw every chunk
    }

    if (table.Dropped() > 0)
        info.warnings.push_back(Num(table.Dropped()) + " dropped (zero-length) frames skipped");
    if (table.Stopped()) info.warnings.push_back(table.StopWarning());
    if (table.Count() == 0)
        throw SerError("AVI has no usable video frames" +
                       (table.Stopped() ? " (" + table.StopWarning() + ")" : std::string()));

    info.frameCount = table.Count();
    h.frameCount = static_cast<int32_t>(std::min<int64_t>(info.frameCount, INT32_MAX));
    details.droppedFrames = table.Dropped();
    info.avi = details;
    f.ids_ = vs.ids;
    f.frameEntry_ = table.TakeEntries();
    f.frameData_ = table.TakeOffsets();
    return f;
}

std::optional<int64_t> AviFile::FrameTimestamp(int64_t index) const {
    if (index < 0 || index >= info_.frameCount)
        throw SerError("frame index " + Num(index) + " out of range (0.." + Num(info_.frameCount - 1) + ")");
    return std::nullopt;
}

Frame AviFile::ReadFrame(int64_t index) const {
    if (index < 0 || index >= info_.frameCount)
        throw SerError("frame index " + Num(index) + " out of range (0.." + Num(info_.frameCount - 1) + ")");

    const int w = info_.header.width, ht = info_.header.height;
    const int64_t chunkBytes = stride_ * ht;
    const Io io(stream_, fileSize_);
    const auto at = [&](int64_t k) { return frameData_[static_cast<size_t>(k)]; };

    // One read: the gap since the previous usable frame (when small; frame 0's was checked at
    // open), this frame's chunk header, its data. Ruling 4: only the gap immediately before the
    // decoded frame is checked here -- walking every earlier gap would turn a jump to the end
    // of a cold 21k-frame ASIAIR capture into a multi-minute stall (one seek per JUNK chunk).
    // The header is verified here because open only checks the index arithmetically.
    const int64_t header = at(index) - 8;
    int64_t start = header;
    if (index > 0 && !allGapsChecked_) {
        start = at(index - 1) + chunkBytes + (chunkBytes & 1);
        if (header - start > kGapReadWhole) {
            CheckGap(io, start, header, ids_, index);
            start = header;
        }
    }
    std::vector<unsigned char> raw(static_cast<size_t>(at(index) + chunkBytes - start));
    if (!io.ReadAt(start, raw.data(), static_cast<int64_t>(raw.size())))
        throw SerError("short read on frame " + Num(index) + ": file ends before the frame does");
    CheckGapBytes(raw.data(), header - start, start, ids_);
    const unsigned char* h = raw.data() + (header - start);
    if (!IsVideoHeader(h, ids_, chunkBytes))
        throw SerError("index entry " + Num(frameEntry_[static_cast<size_t>(index)]) + " points at " +
                       FourccName(h) + "/" + Num(U32(h + 4)) + ", not a video chunk");
    const unsigned char* data = h + 8;

    Frame fr;
    fr.width = w;
    fr.height = ht;
    fr.channels = 1;
    fr.samples.resize(static_cast<size_t>(w) * ht);
    for (int y = 0; y < ht; ++y) {
        const unsigned char* row = data + stride_ * (bottomUp_ ? ht - 1 - y : y);
        uint16_t* out = fr.samples.data() + static_cast<size_t>(w) * y;
        for (int x = 0; x < w; ++x) out[x] = row[x];
    }
    return fr;
}

}  // namespace planetary::ser
