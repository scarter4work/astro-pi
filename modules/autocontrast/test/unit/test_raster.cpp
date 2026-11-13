// AutoContrast: raster decoding and load_raster (core/io/Raster) vs the Python loader, on the synthetic fixtures
// make_goldens.py writes (golden/synthetic/raster: 16-bit PNG/TIFF round trips from test_raster_highdepth.py,
// every Pillow PNG mode, JPEG subsamplings / progressive / gray / CMYK, LZW/PackBits/deflate/tiled TIFF, float TIFF,
// WebP (lossy, lossless, alpha) decode, FITS headers, refusal messages).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "Sha256.h"
#include "core/io/Raster.h"
#include "core/io/Resample.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/raster/" + rel; }

nlohmann::json all_cases() { return ac::test::load_json(golden("cases.json")).at("cases"); }

nlohmann::json case_named(const std::string& name)
{
   for (const auto& c : all_cases())
      if (c.at("name") == name)
         return c;
   throw std::runtime_error("no raster case " + name);
}

std::string sha_of(const ac::Image& im) { return ac::test::sha256_hex(im.data.data(), im.data.size() * sizeof(double)); }

std::string what_of(const std::function<void()>& f)
{
   try {
      f();
   } catch (const std::invalid_argument& e) {
      return e.what();
   }
   return "<no std::invalid_argument>";
}

// a load golden: shape, SHA-256 of the float64 bytes, and (small arrays) the values for a max |d| report
void check_load(const ac::Image& got, const nlohmann::json& want)
{
   const auto& shape = want.at("shape");
   REQUIRE(got.height == shape[0].get<int>());
   REQUIRE(got.width == shape[1].get<int>());
   REQUIRE(got.channels == shape[2].get<int>());
   const std::string sha = sha_of(got);
   if (sha != want.at("sha256_float64").get<std::string>() && want.contains("npy")) {
      const auto ref = ac::test::load_npy(golden(want.at("npy")));
      double worst = 0.0;
      for (size_t i = 0; i < ref.data.size(); ++i)
         worst = std::max(worst, std::fabs(got.data[i] - ref.data[i]));
      WARN("max |d| vs Python = " << worst);
   }
   CHECK(sha == want.at("sha256_float64").get<std::string>());
}

ac::Decoded decode(const std::string& kind, const std::string& path)
{
   if (kind == "jpeg") return ac::decode_jpeg(path);
   if (kind == "png8" || kind == "png16") return ac::decode_png(path);
   if (kind == "tiff") return ac::decode_tiff(path);
   if (kind == "webp") return ac::decode_webp(path);
   throw std::runtime_error("no decoder for " + kind);
}

} // namespace

TEST_CASE("supported_suffixes_is_pythons_allow_list", "[raster]")
{
   const auto want = ac::test::load_json(golden("messages.json")).at("supported");
   std::set<std::string> w;
   for (const auto& s : want)
      w.insert(s.get<std::string>());
   CHECK(ac::supported_suffixes() == w);
   CHECK(w.size() == 11);
}

TEST_CASE("unsupported_suffix_message", "[raster]")
{
   const auto messages = ac::test::load_json(golden("messages.json"));   // named: see test_raster_real.cpp
   for (const auto& r : messages.at("refused")) {
      const std::string path = "/nonexistent/" + r.at("name").get<std::string>();
      const std::string want = r.at("message");
      INFO(path);
      CHECK(what_of([&] { ac::load_raster(path); }) == want);
      CHECK(what_of([&] { ac::load_raster(path, 1600); }) == want);
      CHECK(what_of([&] { ac::image_dimensions(path); }) == want);
   }
}

TEST_CASE("decode_matches_python_decoders", "[raster]")
{
   int n = 0;
   for (const auto& c : all_cases()) {
      if (!c.contains("decode"))
         continue;
      const std::string name = c.at("name"), kind = c.at("kind");
      INFO(name);
      const auto d = decode(kind, golden(c.at("file")));
      const auto& dec = c.at("decode");
      const auto ref = ac::test::load_npy(golden(dec.at("npy")));
      const auto& shape = dec.at("shape");
      REQUIRE(d.height == shape[0].get<int>());
      REQUIRE(d.width == shape[1].get<int>());
      REQUIRE(d.channels == (shape.size() == 3 ? shape[2].get<int>() : 1));
      if (dec.at("mode").is_string())
         CHECK(d.mode == dec.at("mode").get<std::string>());
      const std::string dt = dec.at("dtype");
      size_t diff = 0;
      if (dt == "bool") {
         // Pillow exposes mode "1" as numpy bool over its 0/255 bytes; the value is the truth, and our 0/1
         // must be exactly 0 or 1 (load_raster's float image is the bool cast: 0.0 / 1.0)
         REQUIRE(d.u8.size() == ref.data.size());
         for (size_t i = 0; i < ref.data.size(); ++i)
            diff += d.u8[i] != (ref.data[i] != 0 ? 1 : 0);
      } else if (dt == "uint8") {
         REQUIRE(d.u8.size() == ref.data.size());
         for (size_t i = 0; i < ref.data.size(); ++i)
            diff += d.u8[i] != ref.data[i];
      } else if (dt == "uint16") {
         REQUIRE(d.u16.size() == ref.data.size());
         for (size_t i = 0; i < ref.data.size(); ++i)
            diff += d.u16[i] != ref.data[i];
      } else {
         REQUIRE(dt == "float32");
         REQUIRE(d.f32.size() == ref.data.size());
         for (size_t i = 0; i < ref.data.size(); ++i) {
            const float want = static_cast<float>(ref.data[i]);
            diff += std::memcmp(&d.f32[i], &want, 4) != 0;
         }
      }
      CHECK(diff == 0);
      ++n;
   }
   CHECK(n == 47);
}

TEST_CASE("load_raster_matches_python_on_fixtures", "[raster]")
{
   int n = 0;
   for (const auto& c : all_cases()) {
      for (const auto& l : c.at("loads")) {
         INFO(c.at("name").get<std::string>() << " max_dim=" << l.at("max_dim"));
         const std::string path = golden(c.at("file"));
         const auto got = l.at("max_dim").is_null() ? ac::load_raster(path) : ac::load_raster(path, l.at("max_dim").get<int>());
         check_load(got, l);
         ++n;
      }
   }
   CHECK(n == 96);
}

TEST_CASE("image_dimensions_match_python", "[raster]")
{
   for (const auto& c : all_cases()) {
      INFO(c.at("name").get<std::string>());
      const auto [w, h] = ac::image_dimensions(golden(c.at("file")));
      CHECK(w == c.at("dims")[0].get<int>());
      CHECK(h == c.at("dims")[1].get<int>());
   }
}

TEST_CASE("png16_round_trip_low_byte", "[raster]")
{
   const auto img = ac::load_raster(golden(case_named("png16_rgb").at("file")));
   REQUIRE(img.height == 4);
   REQUIRE(img.width == 6);
   CHECK(img.at(1, 1, 0) == 1.0);
   CHECK(img.at(1, 1, 1) == 40000 / 65535.0);
   CHECK(img.at(1, 1, 2) == 257 / 65535.0);
   const auto gray = ac::load_raster(golden(case_named("png16_gray").at("file")));
   for (int ch = 0; ch < 3; ++ch) {
      CHECK(gray.at(2, 3, ch) == 40000 / 65535.0);
      CHECK(gray.at(0, 0, ch) == 257 / 65535.0);
   }
}

TEST_CASE("tiff16_round_trip", "[raster]")
{
   const auto img = ac::load_raster(golden(case_named("tiff16_rgb").at("file")));
   CHECK(img.at(1, 1, 0) == 1.0);
   CHECK(img.at(1, 1, 1) == 40000 / 65535.0);
   CHECK(img.at(1, 1, 2) == 257 / 65535.0);
   const auto big = ac::load_raster(golden(case_named("tiff16_big").at("file")), 50);
   REQUIRE(big.width == 50);
   REQUIRE(big.height == 25);
   for (double v : big.data)
      REQUIRE(std::fabs(v - 40000 / 65535.0) <= 1e-6);
   CHECK(ac::downsample_factor(golden(case_named("tiff16_big").at("file")), big) == 4.0);
   const auto png = ac::load_raster(golden(case_named("png16_big").at("file")), 50);
   CHECK(ac::downsample_factor(golden(case_named("png16_big").at("file")), png) == 4.0);
}

TEST_CASE("uint16_low_max_not_257x", "[raster]")
{
   // a uint16 image whose brightest pixel is 200 is dark: 200 / 65535, not 200 / 255
   for (const char* name : {"png16_dark_rgb", "png16_dark_gray", "tiff16_dark_rgb"}) {
      INFO(name);
      const auto img = ac::load_raster(golden(case_named(name).at("file")));
      REQUIRE(img.channels == 3);
      for (double v : img.data) {
         REQUIRE(v == 200 / 65535.0);
         REQUIRE(std::fabs(v - 0.003052) < 1e-6);
      }
   }
   for (const char* name : {"png16_darkbig", "tiff16_darkbig"}) {
      INFO(name);
      const auto img = ac::load_raster(golden(case_named(name).at("file")), 50);
      for (double v : img.data)
         REQUIRE(std::fabs(v - 0.003052) < 1e-6);
   }
}

TEST_CASE("jpeg_streamed_load_equals_decode_then_resize", "[raster]")
{
   // load_raster streams JPEG rows through ResizeU8Stream; the non-streamed route decodes the whole image and
   // runs the two-pass resize_u8. Both must give the bytes Pillow gives.
   const std::string path = golden(case_named("jpeg_mid").at("file"));
   const auto d = ac::decode_jpeg(path);
   REQUIRE(d.width == 900);
   REQUIRE(d.height == 700);
   const auto mid = case_named("jpeg_mid");   // named: a range-for over .at() of a temporary dangles
   for (const auto& l : mid.at("loads")) {
      const int md = l.at("max_dim");
      INFO("max_dim " << md);
      const auto streamed = ac::load_raster(path, md);
      check_load(streamed, l);
      const double scale = static_cast<double>(md) / 900;
      const int nw = static_cast<int>(std::nearbyint(900 * scale)), nh = static_cast<int>(std::nearbyint(700 * scale));
      const auto u8 = ac::resize_u8(d.u8.data(), d.width, d.height, 3, nw, nh);
      REQUIRE(streamed.data.size() == u8.size());
      size_t diff = 0;
      for (size_t i = 0; i < u8.size(); ++i)
         diff += streamed.data[i] != u8[i] / 255.0;
      CHECK(diff == 0);
   }
}

TEST_CASE("refusals_name_the_file", "[raster]")
{
   const std::string two = golden(case_named("tiff16_twopage").at("file"));
   CHECK(what_of([&] { ac::load_raster(two); }).rfind("tiff16_twopage.tif: ", 0) == 0);
   const std::string planar = golden(case_named("tiff16_planar").at("file"));
   CHECK(what_of([&] { ac::decode_tiff(planar); }).rfind("tiff16_planar.tif: ", 0) == 0);
   const std::string fits = golden(case_named("fits_cube").at("file"));
   CHECK(what_of([&] { ac::load_raster(fits); }).rfind("fits_cube.fits: ", 0) == 0);
   // pypng's chunk parser refuses a malformed sBIT: Python's message, byte for byte
   const auto bad = case_named("png16_sbit_badlen");
   const std::string sbit = golden(bad.at("file"));
   CHECK(what_of([&] { ac::load_raster(sbit); }) == bad.at("python_error").get<std::string>());
}

TEST_CASE("palette_gray_alpha_and_sbit_load_as_corrected", "[raster]")
{
   // reference bugs fixed on both sides (Task 10 review): palette PNGs load their colours, gray + alpha loads as
   // three-channel gray, 16-bit sBIT PNGs are not shifted down
   const auto p = ac::load_raster(golden(case_named("png8_p_short").at("file")));
   REQUIRE(p.channels == 3);
   CHECK(p.at(0, 0, 0) == 200 / 255.0);   // index 0 -> (200, 10, 10)
   CHECK(p.at(0, 0, 1) == 10 / 255.0);
   bool past_plte = false;                 // indices past the 4-entry PLTE are Pillow's (0, 0, 0)
   for (int x = 0; x < p.width; ++x)
      past_plte = past_plte || (p.at(0, x, 0) == 0.0 && p.at(0, x, 1) == 0.0 && p.at(0, x, 2) == 0.0);
   CHECK(past_plte);
   const auto la = ac::load_raster(golden(case_named("png8_la").at("file")));
   REQUIRE(la.channels == 3);
   for (size_t i = 0; i < la.data.size(); i += 3)
      REQUIRE((la.data[i] == la.data[i + 1] && la.data[i] == la.data[i + 2]));
   const auto raw = ac::decode_png(golden(case_named("png16_sbit12_gray").at("file")));
   const auto sb = ac::load_raster(golden(case_named("png16_sbit12_gray").at("file")));
   REQUIRE(sb.channels == 3);
   for (size_t i = 0; i < raw.u16.size(); ++i)
      REQUIRE(sb.data[i * 3] == raw.u16[i] / 65535.0);
}

TEST_CASE("corrupt_files_raise_instead_of_loading", "[raster]")
{
   // written into the working directory (the build tree under ctest) and removed however the test exits
   struct Scratch {
      std::vector<std::string> files;
      ~Scratch()
      {
         for (const auto& f : files)
            std::remove(f.c_str());
      }
   } scratch;
   auto truncated = [&](const std::string& src, const std::string& dst, size_t cut) {
      scratch.files.push_back(dst);
      std::ifstream in(src, std::ios::binary);
      std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      REQUIRE(bytes.size() > cut);
      bytes.resize(bytes.size() - cut);
      std::ofstream(dst, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
   };
   // the 16-bit PNG path never falls back to an 8-bit decode (Python: "<name>: could not decode 16-bit PNG: ...")
   truncated(golden(case_named("png16_rand_interlaced").at("file")), "raster_bad16.png", 40);
   CHECK(what_of([] { ac::load_raster("raster_bad16.png"); }).rfind("raster_bad16.png: could not decode 16-bit PNG: ", 0) == 0);
   truncated(golden(case_named("png8_rgb").at("file")), "raster_bad8.png", 40);
   CHECK_THROWS(ac::load_raster("raster_bad8.png"));
   truncated(golden(case_named("jpeg_444").at("file")), "raster_bad.jpg", 200);
   CHECK_THROWS(ac::load_raster("raster_bad.jpg"));
   CHECK_THROWS(ac::decode_jpeg("raster_bad.jpg"));
   truncated(golden(case_named("tiff16_deflate_pred").at("file")), "raster_bad.tif", 300);
   CHECK_THROWS(ac::load_raster("raster_bad.tif"));
   truncated(golden(case_named("webp_mid").at("file")), "raster_bad.webp", 500);
   CHECK_THROWS(ac::load_raster("raster_bad.webp"));
   CHECK_THROWS_AS(ac::load_raster("raster_missing.jpg"), std::runtime_error);
}
