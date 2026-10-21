#pragma once
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "FrameSource.h"

namespace planetary::ser {

// Reader for uncompressed 8-bit grey AVI (AVI 1.0 and OpenDML/AVI 2.0): BI_RGB with the
// identity grey palette, or Y800/GREY/Y8. The header and index are untrusted: every
// offset is 64-bit and every size is checked against the file before allocation. Open
// builds the frame offset table from the index (OpenDML indx -> ix##, else idx1, else a
// linear movi scan) and never reads frame data. Index entries are checked arithmetically
// at open (inside the file, padded frame size, ascending, non-overlapping) and the chunk
// headers of the first, middle and last frames are read; ReadFrame verifies each frame's
// own chunk header in the same read as its data. Frames are returned top-down, 1 channel.
// Opened as Mono with bayerSource "none".
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

    // Throws SerError when index is out of range or the frame cannot be read.
    Frame ReadFrame(int64_t index) const override;

private:
    AviFile() = default;

    std::string path_;
    mutable std::ifstream stream_;
    FileInfo info_;
    int64_t fileSize_ = 0;
    int64_t stride_ = 0;      // DIB row bytes, padded to 4
    bool bottomUp_ = false;   // biHeight > 0
    std::vector<int64_t> frameData_;   // file offset of each usable frame's chunk data
    std::vector<int64_t> frameEntry_;  // its index entry number (zero-length entries counted)
    std::string videoDigits_ = "00";   // stream number in the video chunk ids ("00db")
};

}  // namespace planetary::ser
