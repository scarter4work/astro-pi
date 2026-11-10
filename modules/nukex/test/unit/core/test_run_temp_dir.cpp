#include "catch_amalgamated.hpp"
#include "nukex/core/run_temp_dir.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using nukex::RunTempDir;
namespace fs = std::filesystem;

namespace {
struct ScratchBase {
    fs::path dir;
    ScratchBase() {
        dir = fs::temp_directory_path() / ("nukex_rtd_test_" + std::to_string(::getpid()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }
    ~ScratchBase() { fs::remove_all(dir); }
};
}  // namespace

TEST_CASE("RunTempDir: private, unpredictable directory under the base", "[run_temp_dir]") {
    ScratchBase base;
    RunTempDir a(base.dir.string());
    RunTempDir b(base.dir.string());
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(a.path() != b.path());                       // never a fixed path
    REQUIRE(fs::path(a.path()).parent_path() == base.dir);
    REQUIRE(fs::path(a.path()).filename().string().rfind("nukex-run-", 0) == 0);

    struct stat st{};
    REQUIRE(::lstat(a.path().c_str(), &st) == 0);
    REQUIRE(S_ISDIR(st.st_mode));
    REQUIRE((st.st_mode & 0777) == 0700);                // owner only
    REQUIRE(st.st_uid == ::getuid());

    REQUIRE(a.file("nukex_progress.log") == a.path() + "/nukex_progress.log");
}

TEST_CASE("RunTempDir: removed with its files when the run ends", "[run_temp_dir]") {
    ScratchBase base;
    std::string dir;
    {
        RunTempDir d(base.dir.string());
        REQUIRE(d.ok());
        dir = d.path();
        std::ofstream(d.file("nukex_progress.log")) << "x\n";
        std::ofstream(d.file("nukex_heartbeat.txt")) << "y\n";
        REQUIRE(fs::exists(d.file("nukex_progress.log")));
    }
    REQUIRE_FALSE(fs::exists(dir));
    REQUIRE(fs::is_empty(base.dir));
}

TEST_CASE("RunTempDir: cleanup removes a symlink, never its target", "[run_temp_dir]") {
    ScratchBase base;
    const fs::path victim = base.dir / "victim.txt";
    std::ofstream(victim) << "keep\n";
    {
        RunTempDir d(base.dir.string());
        REQUIRE(d.ok());
        fs::create_symlink(victim, d.file("link"));
    }
    REQUIRE(fs::exists(victim));
}

TEST_CASE("RunTempDir: an unusable base gives no directory and no paths", "[run_temp_dir]") {
    RunTempDir d("/nonexistent/nukex/base");
    REQUIRE_FALSE(d.ok());
    REQUIRE(d.path().empty());
    REQUIRE(d.file("nukex_progress.log").empty());
    RunTempDir e("");
    REQUIRE_FALSE(e.ok());
}
