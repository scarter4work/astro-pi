// gdg::write_file_atomic (core/AtomicFile.h, Ruling R10): unique per-writer temporaries, so
// concurrent writers to one path never publish a partial file.
#include <catch_amalgamated.hpp>

#include "core/AtomicFile.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "gdg_atomic_XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<std::uint8_t> pattern(size_t n, std::uint8_t seed) {
    std::vector<std::uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(seed + i * 31u);
    return v;
}

/// The file's bytes, or an empty vector with `exists` false when it is not there.
std::vector<std::uint8_t> read_all(const std::string& path, bool& exists) {
    std::ifstream in(path, std::ios::binary);
    exists = static_cast<bool>(in);
    if (!exists) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

size_t temp_entries(const fs::path& dir, const std::string& name) {
    size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().filename().string().rfind(name + ".tmp", 0) == 0) ++n;
    return n;
}

}  // namespace

TEST_CASE("atomic_file_concurrent_writers_never_partial", "[atomic]") {
    TempDir tmp;
    const std::string path = (tmp.path / "shared.bin").string();
    // Different sizes and bytes: any interleaving or truncation is distinguishable from both.
    const std::vector<std::uint8_t> a = pattern(3 << 20, 7), b = pattern((2 << 20) + 12345, 101);
    constexpr int kRounds = 40;

    std::atomic<bool> done{false};
    std::atomic<int> bad_reads{0}, reads{0};
    std::thread reader([&] {
        while (!done.load()) {
            bool exists = false;
            const std::vector<std::uint8_t> got = read_all(path, exists);
            if (!exists) continue;
            ++reads;
            if (got != a && got != b) ++bad_reads;
        }
    });
    std::atomic<int> errors{0};
    auto writer = [&](const std::vector<std::uint8_t>& bytes) {
        for (int i = 0; i < kRounds; ++i) {
            try {
                gdg::write_file_atomic(path, bytes);
            } catch (const std::exception&) {
                ++errors;
            }
        }
    };
    std::thread wa(writer, std::cref(a)), wb(writer, std::cref(b));
    wa.join();
    wb.join();
    while (reads.load() == 0) std::this_thread::yield();  // the reader has checked at least one complete file
    done = true;
    reader.join();

    CHECK(errors == 0);
    CHECK(bad_reads == 0);
    bool exists = false;
    const std::vector<std::uint8_t> final_bytes = read_all(path, exists);
    REQUIRE(exists);
    CHECK((final_bytes == a || final_bytes == b));
    CHECK(temp_entries(tmp.path, "shared.bin") == 0);
    // The published file is world-readable, not mkstemp's 0600.
    struct stat st {};
    REQUIRE(stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0644);
    CHECK(reads.load() > 0);
}
