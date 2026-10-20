#pragma once
#include <cstdint>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace planetary::ser {

class SerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ColorId : int32_t {
    Mono = 0,
    BayerRGGB = 8,
    BayerGRBG = 9,
    BayerGBRG = 10,
    BayerBGGR = 11,
    RGB = 100,
    BGR = 101
};

enum class ByteOrder { Little, Big };

struct Header {
    int32_t luId = 0;
    int32_t colorIdRaw = 0;
    int32_t littleEndianField = 0;
    int32_t width = 0, height = 0, depth = 0, frameCount = 0;
    std::string observer, instrument, telescope;
    int64_t dateTimeLocal = 0, dateTimeUtc = 0;
};

struct FileInfo {
    Header header;
    ColorId colorId = ColorId::Mono;
    int planes = 1;
    int bytesPerSample = 1;
    int64_t frameBytes = 0;
    int64_t frameCount = 0;
    bool hasTimestamps = false;
    ByteOrder byteOrder = ByteOrder::Little;
    std::vector<std::string> warnings;
};

// One decoded frame: interleaved samples, top-down rows, colour always R,G,B
// (BGR files are reordered). Raw values, not normalised.
struct Frame {
    int width = 0, height = 0, channels = 0;
    std::vector<uint16_t> samples;
};

bool IsBayer(ColorId id);
const char* ColorIdName(ColorId id);
// "RGGB", "GRBG", "GBRG", "BGGR"; throws SerError for non-Bayer ids.
const char* BayerPatternKeyword(ColorId id);

// A SerFile is not thread-safe: it owns one stream with a seek position, and
// ReadFrame/FrameTimestamp move it. Use one reader per thread.
class SerFile {
public:
    static constexpr int64_t kHeaderBytes = 178;

    // Throws SerError with the real reason on any malformed or unsupported file.
    static SerFile Open(const std::string& path);

    SerFile(SerFile&&) noexcept = default;
    SerFile& operator=(SerFile&&) noexcept = default;
    SerFile(const SerFile&) = delete;
    SerFile& operator=(const SerFile&) = delete;

    const FileInfo& Info() const { return info_; }

    // Raw 100-ns tick value from the trailer; nullopt when the file has none.
    // Throws SerError when index is out of range.
    std::optional<int64_t> FrameTimestamp(int64_t index) const;

    // Throws SerError when index is out of range or the file is shorter than expected.
    Frame ReadFrame(int64_t index) const;

private:
    SerFile() = default;

    std::string path_;
    mutable std::ifstream stream_;
    FileInfo info_;
    int64_t fileSize_ = 0;
};

}  // namespace planetary::ser
