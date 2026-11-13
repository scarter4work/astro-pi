// AutoContrast: Pillow-exact resizing (core/io/Resample) vs Image.resize answers (golden/synthetic/raster/resize).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/io/Resample.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/raster/" + rel; }

std::vector<uint8_t> as_u8(const ac::test::NpyArray& a)
{
   REQUIRE(a.dtype == "|u1");
   std::vector<uint8_t> v(a.data.size());
   for (size_t i = 0; i < v.size(); ++i)
      v[i] = static_cast<uint8_t>(a.data[i]);
   return v;
}

std::vector<float> as_f32(const ac::test::NpyArray& a)
{
   REQUIRE(a.dtype == "<f4");
   std::vector<float> v(a.data.size());
   for (size_t i = 0; i < v.size(); ++i)
      v[i] = static_cast<float>(a.data[i]);
   return v;
}

// distance in float32 units in the last place (both finite)
int64_t ulps(float a, float b)
{
   int32_t ia, ib;
   std::memcpy(&ia, &a, 4);
   std::memcpy(&ib, &b, 4);
   if (ia < 0) ia = INT32_MIN - ia;
   if (ib < 0) ib = INT32_MIN - ib;
   return std::llabs(static_cast<int64_t>(ia) - ib);
}

std::vector<uint8_t> streamed(const std::vector<uint8_t>& src, int w, int h, int c, int nw, int nh)
{
   ac::ResizeU8Stream s(w, h, c, nw, nh);
   for (int y = 0; y < h; ++y)
      s.push_row(src.data() + static_cast<size_t>(y) * w * c);
   return s.finish();
}

} // namespace

TEST_CASE("resize_u8_matches_pillow", "[resample]")
{
   const auto cases = ac::test::load_json(golden("resize.json")).at("cases");
   int n = 0;
   for (const auto& c : cases) {
      if (c.at("kind") != "u8")
         continue;
      const int w = c.at("w"), h = c.at("h"), ch = c.at("c"), nw = c.at("nw"), nh = c.at("nh");
      INFO(c.at("in").get<std::string>() << " " << w << "x" << h << "x" << ch << " -> " << nw << "x" << nh);
      const auto in = as_u8(ac::test::load_npy(golden(c.at("in"))));
      const auto want = as_u8(ac::test::load_npy(golden(c.at("out"))));
      const auto got = ac::resize_u8(in.data(), w, h, ch, nw, nh);
      REQUIRE(got.size() == want.size());
      size_t diff = 0;
      for (size_t i = 0; i < got.size(); ++i)
         diff += got[i] != want[i];
      CHECK(diff == 0);
      // the streamed horizontal pass is the same function, row by row
      CHECK(streamed(in, w, h, ch, nw, nh) == got);
      ++n;
   }
   REQUIRE(n == 8);
}

TEST_CASE("resize_f32_matches_pillow", "[resample]")
{
   const auto cases = ac::test::load_json(golden("resize.json")).at("cases");
   int n = 0;
   int64_t worst = 0;
   for (const auto& c : cases) {
      if (c.at("kind") != "f32")
         continue;
      const int w = c.at("w"), h = c.at("h"), nw = c.at("nw"), nh = c.at("nh");
      INFO(c.at("in").get<std::string>() << " " << w << "x" << h << " -> " << nw << "x" << nh);
      const auto in = as_f32(ac::test::load_npy(golden(c.at("in"))));
      const auto want = as_f32(ac::test::load_npy(golden(c.at("out"))));
      const auto got = ac::resize_f32(in.data(), w, h, nw, nh);
      REQUIRE(got.size() == want.size());
      for (size_t i = 0; i < got.size(); ++i) {
         REQUIRE(std::isfinite(got[i]));
         const int64_t u = ulps(got[i], want[i]);
         worst = std::max(worst, u);
         CHECK(u <= 1);
      }
      ++n;
   }
   REQUIRE(n == 8);
   WARN("resize_f32 worst |ulp| vs Pillow: " << worst);
}

TEST_CASE("resize_stream_equals_two_pass", "[resample]")
{
   std::mt19937 rng(20261112);
   struct Shape { int w, h, c, nw, nh; };
   const Shape shapes[] = {{311, 257, 3, 97, 80}, {257, 311, 1, 80, 97}, {123, 77, 4, 123, 31},
                           {123, 77, 2, 40, 77}, {3, 450, 3, 1, 150}, {64, 64, 3, 64, 64}, {500, 9, 3, 37, 1}};
   for (const auto& s : shapes) {
      INFO(s.w << "x" << s.h << "x" << s.c << " -> " << s.nw << "x" << s.nh);
      std::vector<uint8_t> src(static_cast<size_t>(s.w) * s.h * s.c);
      for (auto& v : src)
         v = static_cast<uint8_t>(rng() & 0xFF);
      CHECK(streamed(src, s.w, s.h, s.c, s.nw, s.nh) == ac::resize_u8(src.data(), s.w, s.h, s.c, s.nw, s.nh));
   }
}

TEST_CASE("resize_stream_holds_only_the_reduced_rows", "[resample]")
{
   // 4000 x 4000 RGB -> 400 x 400: the vertical pass needs every source row, reduced to 400 columns.
   const int w = 4000, h = 4000, nw = 400, nh = 400;
   ac::ResizeU8Stream s(w, h, 3, nw, nh);
   std::vector<uint8_t> row(static_cast<size_t>(w) * 3, 128);
   for (int y = 0; y < h; ++y)
      s.push_row(row.data());
   CHECK(s.held_bytes() == static_cast<size_t>(nw) * h * 3);
   const auto out = s.finish();
   REQUIRE(out.size() == static_cast<size_t>(nw) * nh * 3);
   for (uint8_t v : out)
      REQUIRE(v == 128);   // a flat field resamples to itself
}

TEST_CASE("resize_rejects_bad_sizes", "[resample]")
{
   const uint8_t px[4] = {1, 2, 3, 4};
   CHECK_THROWS_AS(ac::resize_u8(px, 1, 1, 3, 0, 1), std::invalid_argument);
   CHECK_THROWS_AS(ac::resize_u8(px, 1, 1, 5, 1, 1), std::invalid_argument);
   const float f = 1.0f;
   CHECK_THROWS_AS(ac::resize_f32(&f, 1, 1, 1, 0), std::invalid_argument);
   ac::ResizeU8Stream s(1, 2, 1, 1, 1);
   s.push_row(px);
   CHECK_THROWS_AS(s.finish(), std::logic_error);
}
