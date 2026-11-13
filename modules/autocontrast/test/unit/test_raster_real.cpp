// AutoContrast: the 45 real renders (5 references, 5 amateur, 35 cached) decoded and loaded natively vs Pillow
// (golden/real/<key>.json from make_goldens.py run_real). Needs the render corpus; exits 77 without it.
//
//   jpeg_decode_bitexact_vs_pillow   every JPEG decoded in full, streamed row by row: SHA-256 of the whole uint8
//                                    array and of its first 16 rows equal Pillow's; native dimensions equal.
//   load_raster_matches_python       load_raster(max_dim=1600): shape equal, SHA-256 of the float64 bytes equal;
//                                    on a mismatch the max |d| over the golden 64 x 64 centre crop is printed.
#include <catch_amalgamated.hpp>

#include "DataDir.h"
#include "Npy.h"
#include "Sha256.h"
#include "core/io/Raster.h"

#include <cmath>
#include <string>
#include <vector>

namespace {

std::string real(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/real/" + rel; }

struct Entry {
   std::string key, group, file;
   nlohmann::json g;
   std::string path() const { return ac::test::group_dir(group) + "/" + file; }
};

std::vector<Entry> entries()
{
   std::vector<Entry> out;
   const auto index = ac::test::load_json(real("index.json"));   // named: a range-for over .at() of a temporary dangles
   for (const auto& e : index.at("entries")) {
      Entry x{e.at("key"), e.at("group"), e.at("file"), {}};
      x.g = ac::test::load_json(real(x.key + ".json"));
      out.push_back(std::move(x));
   }
   REQUIRE(out.size() == 45);
   return out;
}

} // namespace

TEST_CASE("jpeg_decode_bitexact_vs_pillow", "[raster][real]")
{
   int exact = 0;
   for (const auto& e : entries()) {
      INFO(e.key);
      const auto& full = e.g.at("decode_full");
      ac::JpegReader r(e.path());
      REQUIRE(e.g.at("pillow").at("mode") == "RGB");
      REQUIRE(r.mode() == "RGB");
      REQUIRE(r.height() == full.at("shape")[0].get<int>());
      REQUIRE(r.width() == full.at("shape")[1].get<int>());
      REQUIRE(r.channels() == 3);
      std::vector<uint8_t> row(static_cast<size_t>(r.width()) * 3);
      std::vector<uint8_t> rows16;
      ac::test::Sha256 all, first16;
      for (int y = 0; y < r.height(); ++y) {
         r.read_row(row.data());
         all.update(row.data(), row.size());
         if (y < 16) {
            first16.update(row.data(), row.size());
            rows16.insert(rows16.end(), row.begin(), row.end());
         }
      }
      const std::string got16 = first16.hex();
      if (got16 != e.g.at("decode_rows16").at("sha256_uint8").get<std::string>()) {
         const auto ref = ac::test::load_npy(real(e.key + ".rows16_cols256.npy"));   // (16, <=256, 3)
         const size_t cols = ref.shape[1];
         size_t ndiff = 0;
         int worst = 0;
         for (size_t y = 0; y < 16; ++y)
            for (size_t x = 0; x < cols; ++x)
               for (size_t c = 0; c < 3; ++c) {
                  const int d = std::abs(int(rows16[(y * r.width() + x) * 3 + c]) - int(ref.data[(y * cols + x) * 3 + c]));
                  ndiff += d != 0;
                  worst = std::max(worst, d);
               }
         WARN(e.key << ": first 16 rows differ from Pillow; in the kept columns " << ndiff << " samples, max |d| " << worst);
      }
      CHECK(got16 == e.g.at("decode_rows16").at("sha256_uint8").get<std::string>());
      const std::string gotall = all.hex();
      CHECK(gotall == full.at("sha256_uint8").get<std::string>());
      exact += gotall == full.at("sha256_uint8").get<std::string>();
      const auto [w, h] = ac::image_dimensions(e.path());
      CHECK(w == e.g.at("image_dimensions")[0].get<int>());
      CHECK(h == e.g.at("image_dimensions")[1].get<int>());
   }
   WARN("full-image decode bit-identical to Pillow on " << exact << " / 45 renders");
}

TEST_CASE("load_raster_matches_python", "[raster][real]")
{
   const int max_dim = ac::test::load_json(real("index.json")).at("max_dim");
   REQUIRE(max_dim == 1600);
   int exact = 0;
   for (const auto& e : entries()) {
      INFO(e.key);
      const auto& want = e.g.at("raster");
      const ac::Image img = ac::load_raster(e.path(), max_dim);
      REQUIRE(img.height == want.at("shape")[0].get<int>());
      REQUIRE(img.width == want.at("shape")[1].get<int>());
      REQUIRE(img.channels == 3);
      const std::string sha = ac::test::sha256_hex(img.data.data(), img.data.size() * sizeof(double));
      if (sha != want.at("sha256_float64").get<std::string>()) {
         const auto crop = ac::test::load_npy(real(e.key + ".crop64.npy"));
         const bool u8 = crop.dtype == "|u1";
         const int y0 = e.g.at("crop64").at("origin_yx")[0], x0 = e.g.at("crop64").at("origin_yx")[1];
         double worst = 0.0;
         for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
               for (int c = 0; c < 3; ++c) {
                  const double r = crop.data[(static_cast<size_t>(y) * 64 + x) * 3 + c];
                  worst = std::max(worst, std::fabs(img.at(y0 + y, x0 + x, c) - (u8 ? r / 255.0 : r)));
               }
         WARN(e.key << ": load_raster differs from Python; max |d| over the 64x64 centre crop = " << worst);
      }
      CHECK(sha == want.at("sha256_float64").get<std::string>());
      exact += sha == want.at("sha256_float64").get<std::string>();
      const auto& dims = e.g.at("image_dimensions");
      CHECK(ac::downsample_factor(e.path(), img) ==
            static_cast<double>(std::max(dims[0].get<int>(), dims[1].get<int>())) / std::max(img.width, img.height));
   }
   WARN("load_raster bit-identical to Python on " << exact << " / 45 renders");
}
