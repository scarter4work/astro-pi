// AutoContrast: palette-class derivation and the chroma-family gate vs the Python goldens
// (synthetic/palette, synthetic/distance/palette_compat.json).
#include <catch_amalgamated.hpp>

#include "Npy.h"
#include "core/fingerprint/Palette.h"

#include <string>
#include <vector>

namespace {

std::string golden(const std::string& rel) { return std::string(AC_TEST_DIR) + "/golden/synthetic/" + rel; }

std::vector<std::string> strings(const nlohmann::json& j)
{
   return j.get<std::vector<std::string>>();
}

} // namespace

TEST_CASE("palette_class_from_filters: 40 golden rows", "[palette]")
{
   const auto rows = ac::test::load_json(golden("palette/from_filters.json"))["rows"];
   REQUIRE(rows.size() == 40);
   for (const auto& r : rows) {
      const auto filters = strings(r["filters"]);
      INFO(r["filters"].dump());
      const std::string got = ac::palette_class_from_filters(filters);
      REQUIRE(got == r["class"].get<std::string>());
      REQUIRE(ac::kPaletteClasses.count(got) == 1);
   }
}

TEST_CASE("palette_class_from_gallery_bands: 40 golden rows", "[palette]")
{
   const auto rows = ac::test::load_json(golden("palette/from_gallery_bands.json"))["rows"];
   REQUIRE(rows.size() == 40);
   for (const auto& r : rows) {
      const auto bands = strings(r["bands"]);
      INFO(r["bands"].dump());
      const std::string got = ac::palette_class_from_gallery_bands(bands);
      REQUIRE(got == r["class"].get<std::string>());
      REQUIRE(ac::kPaletteClasses.count(got) == 1);
   }
}

TEST_CASE("kPaletteClasses is the eight section 4.3 classes", "[palette]")
{
   const std::set<std::string> want = {"LRGB", "RGB", "SHO", "HOO", "HaRGB", "HaOIII-RGB", "L-only", "unknown"};
   REQUIRE(ac::kPaletteClasses == want);
}

TEST_CASE("chroma_family_gate matches the Python matrix; L-only and unknown never compatible", "[palette]")
{
   const auto g = ac::test::load_json(golden("distance/palette_compat.json"));
   const auto classes = strings(g["classes"]);
   REQUIRE(classes.size() == 8);
   for (size_t i = 0; i < classes.size(); ++i)
      for (size_t j = 0; j < classes.size(); ++j) {
         INFO(classes[i] << " vs " << classes[j]);
         REQUIRE(ac::palette_chroma_compatible(classes[i], classes[j]) == g["matrix"][i][j].get<bool>());
      }
   for (const char* c : {"L-only", "unknown", "not-a-class", ""}) {
      REQUIRE_FALSE(ac::palette_chroma_compatible(c, c));
      REQUIRE_FALSE(ac::palette_chroma_compatible(c, "RGB"));
      REQUIRE_FALSE(ac::palette_chroma_compatible("RGB", c));
   }
   REQUIRE(ac::palette_chroma_compatible("HaRGB", "RGB"));
   REQUIRE_FALSE(ac::palette_chroma_compatible("SHO", "HOO"));
}

TEST_CASE("palette: Python str.strip() whitespace and the two non-ASCII upper() folds", "[palette]")
{
   // U+00A0, U+2003, U+3000 and \x1f are whitespace to Python's strip(); a plain letter is not.
   REQUIRE(ac::palette_class_from_filters({"\xc2\xa0Ha\xe2\x80\x83", "\xe3\x80\x80" "OIII\x1f"}) == "HOO");
   // upper(): dotless i -> I (a broadband key), long s -> S ("SII").
   REQUIRE(ac::palette_class_from_gallery_bands({"\xc4\xb1", "B"}) == "RGB");
   REQUIRE(ac::palette_class_from_gallery_bands({"\xc5\xbf" "II", "OIII", "HA"}) == "SHO");
}
