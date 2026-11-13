// AutoContrast: peak memory of load_raster on the largest reference, heic0601a (18000 x 18000 RGB, 44.5 MB
// JPEG). Pillow decodes it whole (18000 x 18000 x 4 bytes = 1.3 GB) before resizing; the native path streams
// decoded rows through the horizontal pass and keeps only 1600 x 18000 x 3 bytes. Peak resident set growth
// (getrusage ru_maxrss, own process so no other test inflates it first) must stay <= 300 MB, and the result
// must still be Python's bytes. Needs the render corpus; exits 77 without it.
#include <catch_amalgamated.hpp>

#include "DataDir.h"
#include "Npy.h"
#include "Sha256.h"
#include "core/io/Raster.h"

#include <string>
#include <sys/resource.h>

namespace {

long max_rss_kib()
{
   struct rusage ru {};
   getrusage(RUSAGE_SELF, &ru);
   return ru.ru_maxrss;   // KiB on Linux
}

} // namespace

TEST_CASE("heic0601a_peak_memory_under_300mb", "[raster][real][perf]")
{
   const auto g = ac::test::load_json(std::string(AC_TEST_DIR) + "/golden/real/reference__heic0601a.json");
   REQUIRE(g.at("image_dimensions")[0] == 18000);
   const std::string path = ac::test::group_dir("reference") + "/" + g.at("file").get<std::string>();
   const long before = max_rss_kib();
   const ac::Image img = ac::load_raster(path, 1600);
   const long after = max_rss_kib();
   const double delta_mb = (after - before) / 1024.0;
   WARN("heic0601a load_raster(1600): peak RSS " << before / 1024.0 << " MB -> " << after / 1024.0 << " MB (+"
                                                  << delta_mb << " MB)");
   CHECK(delta_mb <= 300.0);
   REQUIRE(img.width == 1600);
   REQUIRE(img.height == 1600);
   CHECK(ac::test::sha256_hex(img.data.data(), img.data.size() * sizeof(double)) ==
         g.at("raster").at("sha256_float64").get<std::string>());
}
