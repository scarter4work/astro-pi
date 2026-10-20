#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

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

}  // namespace planetary::testing
