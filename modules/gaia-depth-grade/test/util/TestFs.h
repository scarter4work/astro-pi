// Test-only filesystem helpers shared by the atomic-export tests (GLB, GIF, orbit).
#pragma once

#include <catch_amalgamated.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <sys/types.h>
#include <vector>

namespace gdg::test {

/// A fresh private directory for one test, removed (permissions restored first) afterwards.
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& prefix = "gdg_test") {
        std::string tmpl = (std::filesystem::temp_directory_path() / (prefix + "_XXXXXX")).string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        REQUIRE(mkdtemp(buf.data()) != nullptr);
        path = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        for (const auto& e : std::filesystem::recursive_directory_iterator(path, ec))
            std::filesystem::permissions(e.path(), std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// True when the directory of `path` holds no `<name>.tmp*` entry (the atomic writer's temporaries).
inline bool no_temps(const std::string& path) {
    const std::filesystem::path p(path);
    const std::string prefix = p.filename().string() + ".tmp";
    std::error_code ec;
    if (!std::filesystem::exists(p.parent_path(), ec)) return true;
    for (const auto& e : std::filesystem::directory_iterator(p.parent_path()))
        if (e.path().filename().string().rfind(prefix, 0) == 0) return false;
    return true;
}


/// The mode write_file_atomic must publish: 0666 & ~umask, the umask read from the `Umask:` line
/// of /proc/self/status (the library reads the same source; umask(2) itself would mutate the process).
inline mode_t expected_published_mode() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("Umask:", 0) == 0) return static_cast<mode_t>(0666 & ~std::stoul(line.substr(6), nullptr, 8));
    FAIL("no Umask: line in /proc/self/status");
    return 0;
}
/// The what() of the exception `f` throws, "" when it throws nothing.
inline std::string thrown_message(const std::function<void()>& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace gdg::test
