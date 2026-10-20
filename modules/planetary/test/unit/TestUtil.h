#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "SerWriter.h"

namespace planetary::testing {

// Per-process temp directory, removed when the test binary exits.
class TempDir {
public:
    TempDir()
        : dir_(std::filesystem::temp_directory_path() /
               ("planetary_ser_test_" + std::to_string(::getpid()))) {
        std::filesystem::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string NewPath(const std::string& ext = ".ser") {
        return (dir_ / ("f" + std::to_string(counter_++) + ext)).string();
    }

private:
    std::filesystem::path dir_;
    std::atomic<int> counter_{0};
};

inline std::string TempPath() {
    static TempDir dir;
    return dir.NewPath();
}

inline SerSpec Spec(int color, int w, int h, int depth, int frames, int le = 0) {
    SerSpec s;
    s.colorId = color; s.width = w; s.height = h; s.depth = depth; s.frameCount = frames;
    s.littleEndianField = le;
    return s;
}

// 16-bit samples to bytes, little- or big-endian.
inline std::vector<uint8_t> Pack16(const std::vector<uint16_t>& v, bool bigEndian) {
    std::vector<uint8_t> b;
    for (uint16_t x : v) {
        const uint8_t lo = x & 0xFF, hi = x >> 8;
        if (bigEndian) { b.push_back(hi); b.push_back(lo); }
        else { b.push_back(lo); b.push_back(hi); }
    }
    return b;
}

}  // namespace planetary::testing
