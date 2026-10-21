#pragma once
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "FrameSource.h"

namespace planetary::ser {

bool IsBayer(ColorId id);
const char* ColorIdName(ColorId id);
// "RGGB", "GRBG", "GBRG", "BGGR"; throws SerError for non-Bayer ids.
const char* BayerPatternKeyword(ColorId id);

// A SerFile is not thread-safe: it owns one stream with a seek position, and
// ReadFrame/FrameTimestamp move it. Use one reader per thread.
class SerFile : public FrameSource {
public:
    static constexpr int64_t kHeaderBytes = 178;

    // Throws SerError with the real reason on any malformed or unsupported file.
    static SerFile Open(const std::string& path);

    SerFile(SerFile&&) noexcept = default;
    SerFile& operator=(SerFile&&) noexcept = default;
    SerFile(const SerFile&) = delete;
    SerFile& operator=(const SerFile&) = delete;

    const FileInfo& Info() const override { return info_; }

    // Raw 100-ns tick value from the trailer; nullopt when the file has none.
    // Throws SerError when index is out of range.
    std::optional<int64_t> FrameTimestamp(int64_t index) const override;

    // Throws SerError when index is out of range or the file is shorter than expected.
    Frame ReadFrame(int64_t index) const override;

private:
    SerFile() = default;

    std::string path_;
    mutable std::ifstream stream_;
    FileInfo info_;
    int64_t fileSize_ = 0;
};

}  // namespace planetary::ser
