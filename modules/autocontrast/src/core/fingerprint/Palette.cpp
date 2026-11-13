#include "core/fingerprint/Palette.h"

#include <cstdint>
#include <map>

namespace ac {

const std::set<std::string> kPaletteClasses = {"LRGB", "RGB", "SHO", "HOO", "HaRGB", "HaOIII-RGB", "L-only", "unknown"};

namespace {

// Python str.strip(): whitespace per str.isspace(), on code points (input is UTF-8).
size_t space_len_at(const std::string& s, size_t i)
{
   const unsigned char c = static_cast<unsigned char>(s[i]);
   if ((c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20))
      return 1;
   auto is = [&](const char* seq, size_t n) { return s.compare(i, n, seq, n) == 0; };
   if (is("\xc2\x85", 2) || is("\xc2\xa0", 2) || is("\xe1\x9a\x80", 3) || is("\xe2\x80\xa8", 3) ||
       is("\xe2\x80\xa9", 3) || is("\xe2\x80\xaf", 3) || is("\xe2\x81\x9f", 3) || is("\xe3\x80\x80", 3))
      return s.compare(i, 1, "\xc2", 1) == 0 ? 2 : 3;
   if (s.compare(i, 2, "\xe2\x80", 2) == 0 && i + 2 < s.size()) {
      const unsigned char d = static_cast<unsigned char>(s[i + 2]);
      if (d >= 0x80 && d <= 0x8a)   // U+2000..U+200A
         return 3;
   }
   return 0;
}

size_t space_len_before(const std::string& s, size_t end)
{
   for (size_t len = 1; len <= 3 && len <= end; ++len) {
      const size_t i = end - len;
      if (space_len_at(s, i) == len)
         return len;
   }
   return 0;
}

std::string py_strip(const std::string& s)
{
   size_t b = 0, e = s.size();
   while (b < e) {
      const size_t l = space_len_at(s, b);
      if (l == 0 || b + l > e)
         break;
      b += l;
   }
   while (e > b) {
      const size_t l = space_len_before(s, e);
      if (l == 0)
         break;
      e -= l;
   }
   return s.substr(b, e - b);
}

std::string ascii_case(std::string s, bool upper)
{
   for (char& c : s) {
      if (upper && c >= 'a' && c <= 'z')
         c = static_cast<char>(c - 'a' + 'A');
      else if (!upper && c >= 'A' && c <= 'Z')
         c = static_cast<char>(c - 'A' + 'a');
   }
   return s;
}

// str.upper() maps two non-ASCII letters onto ASCII letters that occur in the band tables.
std::string py_upper(const std::string& s)
{
   std::string out;
   for (size_t i = 0; i < s.size();) {
      if (s.compare(i, 2, "\xc4\xb1", 2) == 0) {        // U+0131 dotless i -> I
         out += 'I';
         i += 2;
      } else if (s.compare(i, 2, "\xc5\xbf", 2) == 0) { // U+017F long s -> S
         out += 'S';
         i += 2;
      } else {
         out += s[i++];
      }
   }
   return ascii_case(out, true);
}

const std::set<std::string>& dual_band()
{
   static const std::set<std::string> s = {"hao3", "haoiii", "ha-o3", "ha-oiii", "haoiii-dual", "duo", "duo-band",
                                           "duoband", "dualband", "dual-band", "l-extreme", "l-enhance",
                                           "l-ultimate", "lextreme", "lultimate", "alp-t", "alpt"};
   return s;
}

const std::map<std::string, std::string>& aliases()
{
   static const std::map<std::string, std::string> m = {
      {"ha", "Ha"}, {"h-alpha", "Ha"}, {"halpha", "Ha"}, {"h_alpha", "Ha"}, {"oiii", "OIII"}, {"o3", "OIII"},
      {"sii", "SII"}, {"s2", "SII"}, {"l", "L"}, {"lum", "L"}, {"luminance", "L"}, {"r", "R"}, {"red", "R"},
      {"g", "G"}, {"green", "G"}, {"b", "B"}, {"blue", "B"}};
   return m;
}

bool has(const std::set<std::string>& s, const char* k)
{
   return s.count(k) != 0;
}

} // namespace

std::string palette_class_from_filters(const std::vector<std::string>& filters)
{
   if (filters.empty())
      return "RGB";

   std::set<std::string> bands;
   for (const std::string& raw : filters) {
      const std::string key = ascii_case(py_strip(raw), false);
      if (dual_band().count(key)) {
         bands.insert("Ha");
         bands.insert("OIII");
      } else {
         auto it = aliases().find(key);
         if (it == aliases().end())
            return "unknown";
         bands.insert(it->second);
      }
   }

   std::set<std::string> narrow, broad;
   for (const char* n : {"Ha", "OIII", "SII"})
      if (has(bands, n))
         narrow.insert(n);
   for (const char* n : {"R", "G", "B"})
      if (has(bands, n))
         broad.insert(n);
   const bool has_lum = has(bands, "L");
   const std::set<std::string> sho = {"SII", "Ha", "OIII"}, hoo = {"Ha", "OIII"}, rgb = {"R", "G", "B"}, ha = {"Ha"};

   if (narrow == sho && broad.empty())
      return "SHO";
   if (narrow == hoo && broad.empty())
      return "HOO";
   if (narrow == hoo && broad == rgb)
      return "HaOIII-RGB";
   if (narrow == ha && broad == rgb)
      return "HaRGB";
   if (broad == rgb)
      return has_lum ? "LRGB" : "RGB";
   if (has_lum && broad.empty() && narrow.empty())
      return "L-only";
   return "unknown";
}

std::string palette_class_from_gallery_bands(const std::vector<std::string>& bands)
{
   static const std::set<std::string> broadband = {"U", "B", "V", "R", "I", "Z", "Y", "G", "J", "H", "K", "W", "L", "M"};
   static const std::map<std::string, std::string> narrowband = {
      {"HA", "Ha"}, {"H-ALPHA", "Ha"}, {"HALPHA", "Ha"}, {"H_ALPHA", "Ha"}, {"OIII", "OIII"}, {"O-III", "OIII"},
      {"O3", "OIII"}, {"[O III]", "OIII"}, {"[OIII]", "OIII"}, {"SII", "SII"}, {"S-II", "SII"}, {"S2", "SII"},
      {"[S II]", "SII"}, {"[SII]", "SII"}};
   if (bands.empty())
      return "unknown";

   std::set<std::string> broad, narrow;
   for (const std::string& raw : bands) {
      const std::string key = py_upper(py_strip(raw));
      if (key.empty())
         continue;
      auto it = narrowband.find(key);
      if (it != narrowband.end())
         narrow.insert(it->second);
      else if (broadband.count(key))
         broad.insert(key);
   }

   if (broad.size() >= 2)
      return "RGB";
   if (broad.empty()) {
      if (narrow == std::set<std::string>{"SII", "Ha", "OIII"})
         return "SHO";
      if (narrow == std::set<std::string>{"Ha", "OIII"})
         return "HOO";
   }
   return "unknown";
}

bool palette_chroma_compatible(const std::string& a, const std::string& b)
{
   static const std::map<std::string, std::string> family = {
      {"RGB", "broadband"}, {"LRGB", "broadband"}, {"HaRGB", "broadband"}, {"HaOIII-RGB", "broadband"},
      {"SHO", "sho"},       {"HOO", "hoo"}};
   auto fa = family.find(a);
   auto fb = family.find(b);
   return fa != family.end() && fb != family.end() && fa->second == fb->second;
}

} // namespace ac
