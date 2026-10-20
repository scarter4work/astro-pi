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
    auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t b = 0, e = s.size();
    while (b < e && blank(s[b])) ++b;
    while (e > b && blank(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string Num(int64_t v) { return std::to_string(v); }

uint16_t Sample16(const unsigned char* p, ByteOrder order) {
    return order == ByteOrder::Little
               ? static_cast<uint16_t>(p[0] | p[1] << 8)
               : static_cast<uint16_t>(p[1] | p[0] << 8);
}

uint16_t MaxSample16(const std::vector<unsigned char>& bytes, ByteOrder order) {
    uint16_t m = 0;
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) m = std::max(m, Sample16(&bytes[i], order));
    return m;
}
}  // namespace

bool IsBayer(ColorId id) {
    return id == ColorId::BayerRGGB || id == ColorId::BayerGRBG ||
           id == ColorId::BayerGBRG || id == ColorId::BayerBGGR;
}

const char* ColorIdName(ColorId id) {
    switch (id) {
        case ColorId::Mono: return "MONO";
        case ColorId::BayerRGGB: return "BAYER_RGGB";
        case ColorId::BayerGRBG: return "BAYER_GRBG";
        case ColorId::BayerGBRG: return "BAYER_GBRG";
        case ColorId::BayerBGGR: return "BAYER_BGGR";
        case ColorId::RGB: return "RGB";
        case ColorId::BGR: return "BGR";
    }
    throw SerError("unknown ColorId " + Num(static_cast<int32_t>(id)));
}

const char* BayerPatternKeyword(ColorId id) {
    switch (id) {
        case ColorId::BayerRGGB: return "RGGB";
        case ColorId::BayerGRBG: return "GRBG";
        case ColorId::BayerGBRG: return "GBRG";
        case ColorId::BayerBGGR: return "BGGR";
        default: throw SerError(std::string(ColorIdName(id)) + " is not a Bayer pattern");
    }
}

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

    if (!recovered) {
        // Anything after the declared frames is either the 8-byte-per-frame timestamp
        // trailer or unknown data. It is a trailer only when it is exactly 8N bytes, or
        // is at least 8N but too short to hold another whole frame (trailing padding);
        // otherwise it may be frame pixels and must never be read as ticks. Fewer than
        // 8N bytes is a trailer cut short: no timestamps either way.
        const int64_t leftover = f.fileSize_ - kHeaderBytes - info.frameCount * info.frameBytes;
        const int64_t trailer = 8 * info.frameCount;
        if (leftover == trailer || (leftover > trailer && leftover < info.frameBytes)) {
            info.hasTimestamps = true;
        } else if (leftover > 0 && leftover < trailer) {
            info.warnings.push_back("timestamp trailer is incomplete (" + Num(leftover) + " of " +
                                    Num(trailer) + " bytes); no timestamps");
        } else if (leftover > 0) {
            info.warnings.push_back(Num(info.frameCount) +
                                    " frames declared, file holds extra data after them (" +
                                    Num(leftover) + " bytes); ignoring it, no timestamps");
        }
    }

    // Samples below the container's full width cannot exceed 2^depth - 1; frame 0 is
    // checked against that. Byte order (2-byte samples): the header flag is trusted,
    // unless frame 0 breaks the limit under the flag's order but not under the other
    // order; then the other order is used and reported. If frame 0 still exceeds the
    // limit, the declared depth is wrong and every frame is normalised by the full
    // container (8 or 16 bits) instead, so the samples stay within [0,1].
    info.normDepth = h.depth;
    const int containerBits = 8 * info.bytesPerSample;
    if (h.depth < containerBits) {
        std::vector<unsigned char> raw0(static_cast<size_t>(info.frameBytes));
        f.stream_.clear();
        f.stream_.seekg(static_cast<std::streamoff>(kHeaderBytes));
        f.stream_.read(reinterpret_cast<char*>(raw0.data()), static_cast<std::streamsize>(raw0.size()));
        if (!f.stream_) throw SerError("failed to read frame 0 from " + path);
        const uint16_t limit = static_cast<uint16_t>((1u << h.depth) - 1);
        uint16_t max0 = 0;
        if (info.bytesPerSample == 2) {
            const ByteOrder other = info.byteOrder == ByteOrder::Little ? ByteOrder::Big : ByteOrder::Little;
            max0 = MaxSample16(raw0, info.byteOrder);
            if (max0 > limit && MaxSample16(raw0, other) <= limit) {
                const char* flagName = info.byteOrder == ByteOrder::Little ? "little-endian" : "big-endian";
                const char* otherName = other == ByteOrder::Little ? "little-endian" : "big-endian";
                info.warnings.push_back(std::string("byte order: header flag says ") + flagName +
                                        ", data is " + otherName + "; using " + otherName);
                info.byteOrder = other;
                max0 = MaxSample16(raw0, other);
            }
        } else {
            for (unsigned char b : raw0) max0 = std::max<uint16_t>(max0, b);
        }
        if (max0 > limit) {
            info.normDepth = containerBits;
            info.warnings.push_back("samples exceed the declared " + Num(h.depth) + "-bit depth (frame 0 max " +
                                    Num(max0) + "); normalising to the full " + Num(containerBits) +
                                    "-bit range");
        }
    }
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

Frame SerFile::ReadFrame(int64_t index) const {
    if (index < 0 || index >= info_.frameCount)
        throw SerError("frame index " + Num(index) + " out of range (0.." +
                       Num(info_.frameCount - 1) + ")");

    std::vector<unsigned char> raw(static_cast<size_t>(info_.frameBytes));
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(kHeaderBytes + index * info_.frameBytes));
    stream_.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!stream_ || stream_.gcount() != static_cast<std::streamsize>(raw.size()))
        throw SerError("short read on frame " + Num(index) + ": file ends before the frame does");

    Frame fr;
    fr.width = info_.header.width;
    fr.height = info_.header.height;
    fr.channels = info_.planes;
    fr.samples.resize(static_cast<size_t>(info_.frameBytes / info_.bytesPerSample));
    if (info_.bytesPerSample == 1) {
        for (size_t i = 0; i < fr.samples.size(); ++i) fr.samples[i] = raw[i];
    } else {
        for (size_t i = 0; i < fr.samples.size(); ++i) fr.samples[i] = Sample16(&raw[2 * i], info_.byteOrder);
    }
    if (info_.colorId == ColorId::BGR)
        for (size_t i = 0; i + 2 < fr.samples.size(); i += 3) std::swap(fr.samples[i], fr.samples[i + 2]);
    return fr;
}

}  // namespace planetary::ser
