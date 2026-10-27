#pragma once
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace planetary::testing {

struct SerSpec {
    int32_t colorId = 0, littleEndianField = 0, width = 0, height = 0, depth = 8, frameCount = 0;
    int64_t dateTimeUtc = 0;
    std::string observer, instrument, telescope;
    std::string fileId = "LUCAM-RECORDER";
};

inline void PutI32(std::vector<uint8_t>& b, size_t off, int32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
}
inline void PutI64(std::vector<uint8_t>& b, size_t off, int64_t v) {
    for (int i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i));
}
inline void PutText(std::vector<uint8_t>& b, size_t off, const std::string& s) {
    std::memcpy(b.data() + off, s.data(), std::min<size_t>(s.size(), 40));
}

// Writes the exact 178-byte SER header, raw frame bytes, then optional trailer ticks.
inline void WriteSer(const std::string& path, const SerSpec& s,
                     const std::vector<std::vector<uint8_t>>& frameBytes,
                     const std::vector<int64_t>& trailerTicks = {}) {
    std::vector<uint8_t> h(178, 0);
    std::memcpy(h.data(), s.fileId.data(), std::min<size_t>(s.fileId.size(), 14));
    PutI32(h, 14, 0);
    PutI32(h, 18, s.colorId);
    PutI32(h, 22, s.littleEndianField);
    PutI32(h, 26, s.width);
    PutI32(h, 30, s.height);
    PutI32(h, 34, s.depth);
    PutI32(h, 38, s.frameCount);
    PutText(h, 42, s.observer);
    PutText(h, 82, s.instrument);
    PutText(h, 122, s.telescope);
    PutI64(h, 162, 0);
    PutI64(h, 170, s.dateTimeUtc);

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
    for (const auto& f : frameBytes)
        out.write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    for (int64_t t : trailerTicks) {
        std::vector<uint8_t> b(8);
        PutI64(b, 0, t);
        out.write(reinterpret_cast<const char*>(b.data()), 8);
    }
    out.close();
    if (!out) throw std::runtime_error("WriteSer: cannot write " + path + " (disk full?)");
}

}  // namespace planetary::testing
