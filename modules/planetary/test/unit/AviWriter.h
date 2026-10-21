#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Test-only AVI writer: real RIFF structures (AVI 1.0 idx1, OpenDML indx/ix00 + AVIX
// segments, unindexed), plus knobs for every malformed case the reader must handle.
namespace planetary::testing {

struct AviSpec {
    int width = 0, height = 0;
    int bitCount = 8;
    std::string fourcc;  // "" = BI_RGB (with a 256-entry palette when bitCount == 8)
    bool bottomUp = false;
    bool identityPalette = true;
    int badPaletteEntry = -1;  // >= 0: that palette entry becomes (200, 0, 0)
    enum Index { Idx1, OpenDml, None } index = Idx1;
    int framesPerRiff = 0;  // > 0 splits the frames into RIFF AVIX segments
    double fps = 25;
    std::vector<int> zeroLengthFrames;  // written as empty 00db chunks (dropped frames)
    bool paletteChangeChunk = false;    // a 00pc chunk (indexed in idx1 when in segment 0)
    int paletteChangeAfter = 0;         // ...after this frame; -1 = at the start of the first movi
    bool palChangesFlag = false;        // strh dwFlags AVISF_VIDEO_PALCHANGES
    int gapJunkAfter = -1;              // after this frame (before any palette change there):
    int gapJunkChunks = 1;              //   this many JUNK chunks
    size_t gapJunkChunkBytes = 0;       //   of this many data bytes each
    int64_t truncateBytes = 0;          // cut this many bytes off the end of the file
    int corruptIndexEntry = -1;         // that frame's index entries are corrupted as below
    enum Corruption { PastEnd, WrongSize } corruption = PastEnd;  // offset past EOF / size + 2
    int recNesting = 0;                 // frames of each movi wrapped in this many LIST 'rec '
    size_t trailingJunk = 0;            // bytes after the last RIFF segment
    bool idx1Absolute = false;          // idx1 offsets from the file start (as ASIAIR writes them)
    bool openDmlIdx1 = true;            // OpenDml: also write idx1 for the first segment
    bool strhRate = true;               // false: strh rate/scale 0, so avih carries the frame rate
    std::string segmentForm = "AVIX";   // RIFF form of the segments after the first
    bool asiairAvixSize = false;        // segment 1 RIFF and movi sizes 8 too large (ASIAIR)
};

namespace avi_detail {

inline void Put16(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 2; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void Put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void Put64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void Set32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void Set64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i));
}
inline uint32_t U32At(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint32_t>(b[off] | b[off + 1] << 8 | b[off + 2] << 16 | static_cast<uint32_t>(b[off + 3]) << 24);
}
inline void PutFourcc(std::vector<uint8_t>& b, const std::string& f) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(i < static_cast<int>(f.size()) ? f[i] : ' '));
}
// Starts a chunk (or LIST/RIFF when `type` is given); returns the size field's offset.
inline size_t Begin(std::vector<uint8_t>& b, const std::string& id, const std::string& type = "") {
    PutFourcc(b, id);
    const size_t sizeAt = b.size();
    Put32(b, 0);
    if (!type.empty()) PutFourcc(b, type);
    return sizeAt;
}
// Patches the size field and pads the chunk to an even length.
inline void End(std::vector<uint8_t>& b, size_t sizeAt) {
    Set32(b, sizeAt, static_cast<uint32_t>(b.size() - sizeAt - 4));
    if (b.size() & 1) b.push_back(0);
}

}  // namespace avi_detail

// `topDownFrames` holds width*height*bitCount/8 bytes per frame, top row first.
inline void WriteAvi(const std::string& path, const AviSpec& s,
                     const std::vector<std::vector<uint8_t>>& topDownFrames) {
    using namespace avi_detail;
    const int bytesPerPixel = s.bitCount / 8;
    const size_t rowBytes = static_cast<size_t>(s.width) * bytesPerPixel;
    const size_t stride = (rowBytes + 3) & ~size_t(3);
    const uint32_t frameBytes = static_cast<uint32_t>(stride * s.height);
    const int n = static_cast<int>(topDownFrames.size());
    const int perRiff = s.framesPerRiff > 0 ? s.framesPerRiff : (n > 0 ? n : 1);
    const int segments = n > 0 ? (n + perRiff - 1) / perRiff : 1;
    const bool openDml = s.index == AviSpec::OpenDml;
    const bool idx1 = s.index == AviSpec::Idx1 || (openDml && s.openDmlIdx1);  // as real OpenDML files do
    auto isZero = [&](int f) {
        for (int z : s.zeroLengthFrames) if (z == f) return true;
        return false;
    };

    std::vector<uint8_t> b;
    const size_t riff0 = Begin(b, "RIFF", "AVI ");

    // ---- hdrl
    const size_t hdrl = Begin(b, "LIST", "hdrl");
    const size_t avih = Begin(b, "avih");
    Put32(b, static_cast<uint32_t>(std::lround(1e6 / s.fps)));  // dwMicroSecPerFrame
    Put32(b, 0);                                                 // dwMaxBytesPerSec
    Put32(b, 0);                                                 // dwPaddingGranularity
    Put32(b, idx1 ? 0x10u : 0u);                                 // dwFlags (AVIF_HASINDEX)
    Put32(b, static_cast<uint32_t>(std::min(n, perRiff)));      // dwTotalFrames (first RIFF)
    Put32(b, 0);                                                 // dwInitialFrames
    Put32(b, 1);                                                 // dwStreams
    Put32(b, frameBytes);                                        // dwSuggestedBufferSize
    Put32(b, static_cast<uint32_t>(s.width));
    Put32(b, static_cast<uint32_t>(s.height));
    for (int i = 0; i < 4; ++i) Put32(b, 0);
    End(b, avih);

    const size_t strl = Begin(b, "LIST", "strl");
    const size_t strh = Begin(b, "strh");
    PutFourcc(b, "vids");
    PutFourcc(b, s.fourcc.empty() ? "DIB " : s.fourcc);
    Put32(b, s.palChangesFlag ? 0x00010000u : 0u);  // dwFlags
    Put16(b, 0);  // wPriority
    Put16(b, 0);  // wLanguage
    Put32(b, 0);  // dwInitialFrames
    Put32(b, s.strhRate ? 1000000u : 0u);                             // dwScale
    Put32(b, s.strhRate ? static_cast<uint32_t>(std::llround(s.fps * 1000000)) : 0u);  // dwRate
    Put32(b, 0);                                                      // dwStart
    Put32(b, static_cast<uint32_t>(n));                               // dwLength
    Put32(b, frameBytes);                                             // dwSuggestedBufferSize
    Put32(b, 0xFFFFFFFFu);                                            // dwQuality
    Put32(b, 0);                                                      // dwSampleSize
    Put16(b, 0); Put16(b, 0);
    Put16(b, static_cast<uint32_t>(s.width)); Put16(b, static_cast<uint32_t>(s.height));
    End(b, strh);

    const bool palette = s.fourcc.empty() && s.bitCount <= 8;
    const size_t strf = Begin(b, "strf");
    Put32(b, 40);
    Put32(b, static_cast<uint32_t>(s.width));
    Put32(b, static_cast<uint32_t>(s.bottomUp ? s.height : -s.height));
    Put16(b, 1);
    Put16(b, static_cast<uint32_t>(s.bitCount));
    if (s.fourcc.empty()) Put32(b, 0); else PutFourcc(b, s.fourcc);
    Put32(b, frameBytes);  // biSizeImage
    Put32(b, 0); Put32(b, 0);
    Put32(b, palette ? 256 : 0);  // biClrUsed
    Put32(b, 0);
    if (palette) {
        for (int i = 0; i < 256; ++i) {
            uint8_t r = static_cast<uint8_t>(i), g = r, bl = r;
            if (!s.identityPalette) { r = static_cast<uint8_t>(255 - i); g = r; bl = r; }
            if (i == s.badPaletteEntry) { r = 200; g = 0; bl = 0; }
            b.push_back(bl); b.push_back(g); b.push_back(r); b.push_back(0);
        }
    }
    End(b, strf);

    size_t superEntries = 0;  // offset of the first AVISUPERINDEX entry
    if (openDml) {
        const size_t indx = Begin(b, "indx");
        Put16(b, 4);  // wLongsPerEntry
        b.push_back(0);  // bIndexSubType
        b.push_back(0);  // bIndexType = AVI_INDEX_OF_INDEXES
        Put32(b, static_cast<uint32_t>(segments));
        PutFourcc(b, "00db");
        for (int i = 0; i < 3; ++i) Put32(b, 0);
        superEntries = b.size();
        for (int i = 0; i < segments; ++i) { Put64(b, 0); Put32(b, 0); Put32(b, 0); }
        End(b, indx);
    }
    End(b, strl);
    if (openDml) {
        const size_t odml = Begin(b, "LIST", "odml");
        const size_t dmlh = Begin(b, "dmlh");
        Put32(b, static_cast<uint32_t>(n));
        End(b, dmlh);
        End(b, odml);
    }
    End(b, hdrl);

    // ---- movi lists, one per RIFF segment
    struct Idx1Entry { std::string id; uint32_t flags, offset, size; };
    std::vector<Idx1Entry> idx1Entries;
    size_t riff = riff0;
    for (int seg = 0; seg < segments; ++seg) {
        if (seg > 0) {
            End(b, riff);
            if (seg == 2 && s.asiairAvixSize) Set32(b, riff, U32At(b, riff) + 8);
            riff = Begin(b, "RIFF", s.segmentForm);
        }
        const size_t moviList = b.size();
        const size_t movi = Begin(b, "LIST", "movi");
        const size_t moviFourcc = moviList + 8;
        const size_t idx1Base = s.idx1Absolute ? 0 : moviFourcc;  // idx1 points at chunk headers
        const int first = seg * perRiff, last = std::min(n, first + perRiff);

        // ix00 at the start of the movi list, as the ASIAIR writer places it.
        size_t ixEntries = 0;
        if (openDml) {
            const size_t ixAt = b.size();
            const size_t ix = Begin(b, "ix00");
            Put16(b, 2);  // wLongsPerEntry
            b.push_back(0);  // bIndexSubType
            b.push_back(1);  // bIndexType = AVI_INDEX_OF_CHUNKS
            Put32(b, static_cast<uint32_t>(last - first));
            PutFourcc(b, "00db");
            Put64(b, moviList);  // qwBaseOffset
            Put32(b, 0);
            ixEntries = b.size();
            for (int f = first; f < last; ++f) { Put32(b, 0); Put32(b, 0); }
            End(b, ix);
            const size_t e = superEntries + 16 * static_cast<size_t>(seg);
            Set64(b, e, ixAt);
            Set32(b, e + 8, static_cast<uint32_t>(b.size() - ixAt));
            Set32(b, e + 12, static_cast<uint32_t>(last - first));
        }

        auto writePaletteChange = [&] {
            const size_t pcAt = b.size();
            const size_t pc = Begin(b, "00pc");
            b.push_back(0); b.push_back(1); Put16(b, 0);  // bFirstEntry, bNumEntries, wFlags
            b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0);
            End(b, pc);
            if (idx1 && seg == 0) idx1Entries.push_back({"00pc", 0, static_cast<uint32_t>(pcAt - idx1Base), 8});
        };
        if (s.paletteChangeChunk && s.paletteChangeAfter < 0 && seg == 0) writePaletteChange();

        std::vector<size_t> recLists;
        for (int r = 0; r < s.recNesting; ++r) recLists.push_back(Begin(b, "LIST", "rec "));
        for (int f = first; f < last; ++f) {
            const std::vector<uint8_t>& src = topDownFrames[f];
            if (!isZero(f) && src.size() != rowBytes * s.height)
                throw std::invalid_argument("WriteAvi: frame has the wrong byte count");
            const size_t chunkAt = b.size();
            const size_t ck = Begin(b, "00db");
            const uint32_t size = isZero(f) ? 0 : frameBytes;
            if (size) {
                for (int y = 0; y < s.height; ++y) {
                    // Only RGB DIBs are bottom-up; YUV (Y800...) rows are top-down whatever the sign.
                    const int srcRow = s.bottomUp && s.fourcc.empty() ? s.height - 1 - y : y;
                    const uint8_t* row = src.data() + rowBytes * srcRow;
                    b.insert(b.end(), row, row + rowBytes);
                    b.insert(b.end(), stride - rowBytes, uint8_t(0));
                }
            }
            End(b, ck);
            const bool corrupt = f == s.corruptIndexEntry;
            const bool pastEnd = corrupt && s.corruption == AviSpec::PastEnd;
            const uint32_t indexSize = corrupt && s.corruption == AviSpec::WrongSize ? size + 2 : size;
            if (openDml) {
                // dwOffset -> chunk DATA, relative to qwBaseOffset; bit 31 of dwSize marks a
                // delta frame (set on odd frames so the reader's mask is exercised).
                const size_t e = ixEntries + 8 * static_cast<size_t>(f - first);
                Set32(b, e, pastEnd ? 0x7FFFFFF0u : static_cast<uint32_t>(chunkAt + 8 - moviList));
                Set32(b, e + 4, indexSize | (f % 2 ? 0x80000000u : 0u));
            }
            if (idx1 && seg == 0)
                idx1Entries.push_back({"00db", 0x10, pastEnd ? 0x7FFFFFF0u : static_cast<uint32_t>(chunkAt - idx1Base),
                                       indexSize});
            if (f == s.gapJunkAfter) {
                for (int j = 0; j < s.gapJunkChunks; ++j) {
                    const size_t junk = Begin(b, "JUNK");
                    b.insert(b.end(), s.gapJunkChunkBytes, uint8_t(0));
                    End(b, junk);
                }
            }
            if (s.paletteChangeChunk && f == s.paletteChangeAfter) writePaletteChange();
        }
        for (auto r = recLists.rbegin(); r != recLists.rend(); ++r) End(b, *r);
        End(b, movi);
        if (seg == 1 && s.asiairAvixSize) Set32(b, movi, U32At(b, movi) + 8);

        if (idx1 && seg == 0) {
            const size_t ix = Begin(b, "idx1");
            for (const auto& e : idx1Entries) {
                PutFourcc(b, e.id);
                Put32(b, e.flags);
                Put32(b, e.offset);
                Put32(b, e.size);
            }
            End(b, ix);
        }
    }
    End(b, riff);
    if (segments == 2 && s.asiairAvixSize) Set32(b, riff, U32At(b, riff) + 8);

    for (size_t i = 0; i < s.trailingJunk; ++i) b.push_back(static_cast<uint8_t>(i * 37 + 11));
    if (s.truncateBytes > 0) b.resize(b.size() - static_cast<size_t>(s.truncateBytes));

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

}  // namespace planetary::testing
