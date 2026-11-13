// gdg::write_file_atomic (core/AtomicFile.h, Ruling R10): unique per-writer temporaries, so
// concurrent writers to one path never publish a partial file.
#include <catch_amalgamated.hpp>

#include "TestFs.h"
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
    // The published file gets 0666 & ~umask, not mkstemp's 0600.
    struct stat st {};
    REQUIRE(stat(path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == gdg::test::expected_published_mode());
    CHECK(reads.load() > 0);
}

TEST_CASE("atomic_file_umask_from_proc_status", "[atomic]") {
    // The text of /proc/self/status around the Umask: line (octal, as the kernel prints it).
    CHECK(gdg::umask_from_proc_status("Name:\tx\nUmask:\t0022\nState:\tR\n") == std::optional<unsigned>(022));
    CHECK(gdg::umask_from_proc_status("Umask:\t0077\n") == std::optional<unsigned>(077));
    CHECK(gdg::umask_from_proc_status("Umask:\t0\n") == std::optional<unsigned>(0));
    CHECK(gdg::umask_from_proc_status("Umask:\t0777") == std::optional<unsigned>(0777));
    // No line, an empty or non-octal value, trailing text or a value beyond 0777: undetermined.
    for (const char* bad : {"", "Name:\tx\n", "Umask:\n", "Umask:\t\n", "Umask:\t0089\n", "Umask:\tzz\n",
                            "Umask:\t0022 x\n", "Umask:\t01000\n", "Umask:\t-1\n", "xUmask:\t0022\n"}) {
        INFO("status text: " << bad);
        CHECK_FALSE(gdg::umask_from_proc_status(bad).has_value());
    }
    // The running process: the library and the test agree.
    const auto live = gdg::process_umask();
    REQUIRE(live.has_value());
    CHECK(static_cast<mode_t>(0666 & ~*live) == gdg::test::expected_published_mode());
}
