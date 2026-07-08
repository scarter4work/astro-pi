#include "catch_amalgamated.hpp"
#include "png_writer.hpp"
#include "nukex/io/image.hpp"

#include <filesystem>

namespace fs = std::filesystem;
using namespace nukex;

TEST_CASE("write_png creates its parent directory when it doesn't exist",
          "[png_writer]") {
    // Root-cause regression test for the "stretch tests fail on a clean
    // build because build/test/output/ doesn't exist yet" problem: the
    // writer must create its own output directory rather than relying on
    // an external `mkdir -p` before the test binary runs.
    auto dir = fs::temp_directory_path() / "nukex_png_writer_test"
             / "nested" / "does_not_exist_yet";
    fs::remove_all(fs::temp_directory_path() / "nukex_png_writer_test");
    REQUIRE_FALSE(fs::exists(dir));

    Image img(4, 4, 1);
    img.fill(0.5f);

    auto path = dir / "out.png";
    bool ok = test_util::write_png(path.string(), img, /*apply_stretch=*/false);

    REQUIRE(ok);
    REQUIRE(fs::exists(path));

    fs::remove_all(fs::temp_directory_path() / "nukex_png_writer_test");
}

TEST_CASE("write_png_8bit creates its parent directory when it doesn't exist",
          "[png_writer]") {
    auto dir = fs::temp_directory_path() / "nukex_png_writer_test_8bit" / "nested";
    fs::remove_all(fs::temp_directory_path() / "nukex_png_writer_test_8bit");
    REQUIRE_FALSE(fs::exists(dir));

    Image img(4, 4, 1);
    img.fill(0.5f);

    auto path = dir / "out.png";
    bool ok = test_util::write_png_8bit(path.string(), img);

    REQUIRE(ok);
    REQUIRE(fs::exists(path));

    fs::remove_all(fs::temp_directory_path() / "nukex_png_writer_test_8bit");
}
