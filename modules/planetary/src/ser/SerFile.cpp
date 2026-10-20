#include "SerFile.h"

#include <algorithm>
#include <cstring>

namespace planetary::ser {

namespace {
constexpr char kMagic[] = "LUCAM-RECORDER";
constexpr size_t kMagicLen = 14;
constexpr int32_t kMaxDimension = 65536;

int32_t ReadI32(const unsigned char* p) {
    uint32_t v = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                 static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
    return static_cast<int32_t>(v);
}

int64_t ReadI64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return static_cast<int64_t>(v);
}

std::string ReadText(const unsigned char* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    const size_t nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

std::string Num(int64_t v) { return std::to_string(v); }
}  // namespace

SerFile SerFile::Open(const std::string& path) {
    SerFile f;
    f.path_ = path;
    f.stream_.open(path, std::ios::binary);
    if (!f.stream_) throw SerError("cannot open SER file: " + path);

    f.stream_.seekg(0, std::ios::end);
    const std::streamoff end = f.stream_.tellg();
    if (end < 0) throw SerError("cannot determine size of SER file: " + path);
    f.fileSize_ = static_cast<int64_t>(end);
    if (f.fileSize_ < kHeaderBytes)
        throw SerError("file is too small to be a SER file (" + Num(f.fileSize_) +
                       " bytes, header needs " + Num(kHeaderBytes) + ")");

    unsigned char raw[kHeaderBytes];
    f.stream_.seekg(0);
    f.stream_.read(reinterpret_cast<char*>(raw), kHeaderBytes);
    if (!f.stream_) throw SerError("failed to read SER header from " + path);

    if (std::memcmp(raw, kMagic, kMagicLen) != 0)
        throw SerError("not a SER file: header does not start with LUCAM-RECORDER");

    FileInfo& info = f.info_;
    Header& h = info.header;
    h.luId = ReadI32(raw + 14);
    h.colorIdRaw = ReadI32(raw + 18);
    h.littleEndianField = ReadI32(raw + 22);
    h.width = ReadI32(raw + 26);
    h.height = ReadI32(raw + 30);
    h.depth = ReadI32(raw + 34);
    h.frameCount = ReadI32(raw + 38);
    h.observer = ReadText(raw + 42, 40);
    h.instrument = ReadText(raw + 82, 40);
    h.telescope = ReadText(raw + 122, 40);
    h.dateTimeLocal = ReadI64(raw + 162);
    h.dateTimeUtc = ReadI64(raw + 170);

    switch (h.colorIdRaw) {
        case 0: case 8: case 9: case 10: case 11: case 100: case 101:
            info.colorId = static_cast<ColorId>(h.colorIdRaw);
            break;
        default:
            if (h.colorIdRaw >= 16 && h.colorIdRaw <= 19)
                throw SerError("ColorID " + Num(h.colorIdRaw) +
                               " (CMY Bayer) is not supported");
            throw SerError("ColorID " + Num(h.colorIdRaw) + " is not supported");
    }

    if (h.depth < 1 || h.depth > 16)
        throw SerError("invalid PixelDepthPerPlane " + Num(h.depth) + " (must be 1-16)");
    if (h.width <= 0 || h.height <= 0)
        throw SerError("invalid image size " + Num(h.width) + "x" + Num(h.height));
    if (h.width > kMaxDimension || h.height > kMaxDimension)
        throw SerError("image size " + Num(h.width) + "x" + Num(h.height) +
                       " exceeds the supported maximum of " + Num(kMaxDimension));

    info.planes = (info.colorId == ColorId::RGB || info.colorId == ColorId::BGR) ? 3 : 1;
    info.bytesPerSample = h.depth <= 8 ? 1 : 2;
    info.frameBytes = static_cast<int64_t>(h.width) * h.height * info.planes * info.bytesPerSample;
    info.byteOrder = h.littleEndianField == 0 ? ByteOrder::Little : ByteOrder::Big;

    const int64_t dataBytes = f.fileSize_ - kHeaderBytes;
    const int64_t complete = dataBytes / info.frameBytes;
    if (complete < 1)
        throw SerError("file contains no complete frames (" + Num(dataBytes) +
                       " data bytes, one frame is " + Num(info.frameBytes) + ")");

    bool recovered = false;
    if (h.frameCount <= 0) {
        info.frameCount = complete;
        recovered = true;
        info.warnings.push_back("header frame count is " + Num(h.frameCount) +
                                "; using " + Num(complete) + " complete frames found in the file");
    } else if (static_cast<int64_t>(h.frameCount) > complete) {
        info.frameCount = complete;
        recovered = true;
        info.warnings.push_back("header frame count " + Num(h.frameCount) +
                                " exceeds the " + Num(complete) +
                                " complete frames in the file (truncated); using " + Num(complete));
    } else {
        info.frameCount = h.frameCount;
    }

    info.hasTimestamps =
        !recovered && f.fileSize_ >= kHeaderBytes + info.frameCount * info.frameBytes +
                                         8 * info.frameCount;
    return f;
}

std::optional<int64_t> SerFile::FrameTimestamp(int64_t index) const {
    if (index < 0 || index >= info_.frameCount)
        throw SerError("frame index " + Num(index) + " out of range (0.." +
                       Num(info_.frameCount - 1) + ")");
    if (!info_.hasTimestamps) return std::nullopt;

    const int64_t offset = kHeaderBytes + info_.frameCount * info_.frameBytes + 8 * index;
    unsigned char buf[8];
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset));
    stream_.read(reinterpret_cast<char*>(buf), 8);
    if (!stream_) throw SerError("failed to read timestamp for frame " + Num(index));
    return ReadI64(buf);
}

}  // namespace planetary::ser
