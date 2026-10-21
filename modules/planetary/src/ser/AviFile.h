#pragma once
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "FrameSource.h"

namespace planetary::ser {

namespace avi_detail {
// Chunk ids of the chosen video stream NN: "NNdb"/"NNdc" (frames), "NNpc" (palette change),
// "ixNN" (OpenDML standard index).
struct StreamIds {
    char digits[2] = {'0', '0'};
    bool IsFrame(const unsigned char* p) const {
        return p[0] == digits[0] && p[1] == digits[1] && p[2] == 'd' && (p[3] == 'b' || p[3] == 'c');
    }
    bool IsPaletteChange(const unsigned char* p) const {
        return p[0] == digits[0] && p[1] == digits[1] && p[2] == 'p' && p[3] == 'c';
    }
    bool IsIndex(const unsigned char* p) const {
        return p[0] == 'i' && p[1] == 'x' && p[2] == digits[0] && p[3] == digits[1];
    }
};
}  // namespace avi_detail

// Reader for uncompressed 8-bit grey AVI (AVI 1.0 and OpenDML/AVI 2.0): BI_RGB with the
// identity grey palette, or Y800/GREY/Y8. The header and index are untrusted: every
// offset is 64-bit and every size is checked against the file before allocation. Open
// builds the frame offset table from the index (OpenDML indx -> ix##, else idx1, else a
// linear movi scan) and never reads frame data. Index entries are checked arithmetically
// at open (inside the file, padded frame size, ascending, non-overlapping) and the chunk
// headers of the first, middle and last frames are read; ReadFrame verifies each frame's
// own chunk header in the same read as its data. Palette-change chunks are an error: the
// scan, idx1 and the gap before frame 0 are checked at open (every gap when strh declares
// palette changes); ReadFrame checks the gap immediately before the frame it decodes, in the
// same read. A palette change in a gap before an unread frame is not detected (Ruling 4).
// Frames are returned top-down, 1 channel. Opened as Mono with bayerSource "none".
//
// Not thread-safe: it owns one stream with a seek position. Use one reader per thread.
class AviFile : public FrameSource {
public:
    // A run of more zero-length (dropped) frame entries than this ends the frame table.
    static constexpr int64_t kMaxDroppedRun = 65536;
    // Deepest nesting of LIST 'rec ' inside movi accepted by the scan (real files use 1).
    static constexpr int kMaxRecDepth = 4;

    // Throws SerError with the real reason on any malformed or unsupported file.
    static AviFile Open(const std::string& path);

    AviFile(AviFile&&) noexcept = default;
    AviFile& operator=(AviFile&&) noexcept = default;
    AviFile(const AviFile&) = delete;
    AviFile& operator=(const AviFile&) = delete;

    const FileInfo& Info() const override { return info_; }

    // Always nullopt (AVI has no per-frame clock). Throws SerError when index is out of range.
    std::optional<int64_t> FrameTimestamp(int64_t index) const override;

    // Throws SerError when index is out of range, the frame cannot be read, or the index
    // entry does not point at this stream's video chunk.
    Frame ReadFrame(int64_t index) const override;

private:
    AviFile() = default;

    std::string path_;
    mutable std::ifstream stream_;
    FileInfo info_;
    int64_t fileSize_ = 0;
    int64_t stride_ = 0;      // DIB row bytes, padded to 4
    bool bottomUp_ = false;   // BI_RGB with biHeight > 0 (YUV fourccs are always top-down)
    avi_detail::StreamIds ids_;
    std::vector<int64_t> frameData_;   // file offset of each usable frame's chunk data
    std::vector<int64_t> frameEntry_;  // its index entry number (zero-length entries counted)
    // Every gap between frames was checked for palette-change chunks at open (scan, or strh
    // declares palette changes), so ReadFrame need not check the gap before its frame.
    bool allGapsChecked_ = false;
};

}  // namespace planetary::ser
