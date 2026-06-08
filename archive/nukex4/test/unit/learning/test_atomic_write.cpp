#include "catch_amalgamated.hpp"
#include "nukex/learning/atomic_write.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using nukex::learning::atomic_write_file;

namespace {

// Read entire file into a string (binary). Empty string on any error.
std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path make_tmp_dir(const std::string& suffix) {
    fs::path d = fs::temp_directory_path() / ("nukex_test_atomic_write_" + suffix);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

} // namespace

TEST_CASE("atomic_write_file: new file is written", "[learning][atomic_write]") {
    const fs::path dir  = make_tmp_dir("new");
    const fs::path path = dir / "out.txt";
    REQUIRE_FALSE(fs::exists(path));

    const std::string contents = "hello atomic\n";
    REQUIRE(atomic_write_file(path.string(), contents));
    REQUIRE(fs::exists(path));
    REQUIRE(read_file(path) == contents);

    // .tmp sibling should be gone after successful rename.
    REQUIRE_FALSE(fs::exists(path.string() + ".tmp"));

    fs::remove_all(dir);
}

TEST_CASE("atomic_write_file: replaces existing file", "[learning][atomic_write]") {
    const fs::path dir  = make_tmp_dir("replace");
    const fs::path path = dir / "out.txt";

    // Seed the original file with OLD contents.
    {
        std::ofstream f(path, std::ios::binary);
        f << "OLD CONTENT that is longer than the new one";
    }
    REQUIRE(fs::exists(path));

    const std::string new_contents = "NEW";
    REQUIRE(atomic_write_file(path.string(), new_contents));
    REQUIRE(read_file(path) == new_contents);
    REQUIRE_FALSE(fs::exists(path.string() + ".tmp"));

    fs::remove_all(dir);
}

TEST_CASE("atomic_write_file: failure leaves original intact", "[learning][atomic_write]") {
    const fs::path dir  = make_tmp_dir("fail");
    const fs::path path = dir / "file.txt";

    // Write OLD into the target so we can verify it survives the failed write.
    {
        std::ofstream f(path, std::ios::binary);
        f << "OLD";
    }
    REQUIRE(fs::exists(path));

    // Drop write permission on the parent directory: the tmp-file creation
    // (or the rename) must fail, and atomic_write_file must return false.
    // We restore perms BEFORE asserting so a failed assertion doesn't leave
    // the test harness unable to clean up.
    std::error_code ec_perm;
    fs::permissions(dir,
                    fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace,
                    ec_perm);
    REQUIRE_FALSE(ec_perm);

    const bool ok = atomic_write_file(path.string(), "NEW");

    // Restore perms so we can read/delete afterwards.
    fs::permissions(dir,
                    fs::perms::owner_all,
                    fs::perm_options::replace,
                    ec_perm);
    REQUIRE_FALSE(ec_perm);

    // Now it's safe to assert and clean up.
    REQUIRE_FALSE(ok);
    REQUIRE(read_file(path) == "OLD");

    fs::remove_all(dir);
}
