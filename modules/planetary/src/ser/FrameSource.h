#pragma once
#include <cstdint>
#include <memory>
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

enum class Container { Ser, Avi, AviOpenDml };

enum class BayerChoice { Auto, None, RGGB, GRBG, GBRG, BGGR };

// ASIAIR capture-log fields, raw strings as read from the sidecar.
struct AsiairSidecar {
    std::string camera, colourFormat, bayer, flip, gain, exposure, captureArea;
    // The section was [SERViewer] (WriteAsiairSidecar): `camera` is empty, never "SERViewer".
    bool writtenBySERViewer = false;
};

// What became of the `<video>.avi.txt` next to an AVI: none there (nothing at that path), there
// but not a file (a directory, FIFO or dangling link: NotAFile, never opened), there but unreadable
// (I/O error or no section/key lines), there but over the 64 KiB cap, or parsed.
enum class SidecarStatus { Absent, Unreadable, TooLarge, Parsed, NotAFile };

struct AviDetails {
    double frameRate = 0;
    std::string bayerSource;
    SidecarStatus sidecarStatus = SidecarStatus::Absent;
    std::optional<AsiairSidecar> sidecar;  // set exactly when sidecarStatus is Parsed
    std::optional<std::string> filenameStart;
    std::string indexKind;
    int64_t droppedFrames = 0;
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
    // Depth every frame is normalised by: the declared depth, or the full 8/16 bits
    // of the sample container when frame 0 exceeds 2^depth - 1 (reported as a warning).
    int normDepth = 0;
    std::vector<std::string> warnings;
    Container container = Container::Ser;
    std::optional<AviDetails> avi;  // empty for SER files
};

// One decoded frame: interleaved samples, top-down rows, colour always R,G,B
// (BGR files are reordered). Raw values, not normalised.
struct Frame {
    int width = 0, height = 0, channels = 0;
    std::vector<uint16_t> samples;
};

// A source of decoded frames (SER or AVI). Not thread-safe: implementations own
// a stream with a seek position, so use one reader per thread.
class FrameSource {
public:
    virtual ~FrameSource() = default;
    virtual const FileInfo& Info() const = 0;
    // Throws SerError when index is out of range or the file is shorter than expected.
    virtual Frame ReadFrame(int64_t index) const = 0;
    // Raw 100-ns tick value; nullopt when the source has none.
    virtual std::optional<int64_t> FrameTimestamp(int64_t index) const = 0;
};

// Opens a video by content (first 12 bytes), never by extension: SER ("LUCAM-RECORDER"),
// or AVI ("RIFF....AVI "). `bayer` applies to AVI only. Throws SerError.
std::unique_ptr<FrameSource> OpenVideo(const std::string& path,
                                       BayerChoice bayer = BayerChoice::Auto);

}  // namespace planetary::ser
