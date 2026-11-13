#include "core/Json.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <system_error>

namespace ac {
namespace {

// Python float.__repr__ (format 'r'): shortest digits that round-trip; scientific form when the
// decimal exponent is < -4 or >= 16 (decpt <= -4 or decpt > 16 with decpt = exp10 + 1).
void append_float(std::string& out, double v)
{
   if (std::isnan(v)) { out += "NaN"; return; }
   if (std::isinf(v)) { out += v > 0 ? "Infinity" : "-Infinity"; return; }
   if (v == 0.0) { out += std::signbit(v) ? "-0.0" : "0.0"; return; }

   char buf[64];
   const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);   // shortest
   if (r.ec != std::errc())
      throw std::runtime_error("dump_python: float conversion failed");
   std::string s(buf, r.ptr);                      // [-]d[.ddd]e[+-]XX
   size_t p = 0;
   const bool neg = s[0] == '-';
   if (neg) ++p;
   const size_t epos = s.find('e');
   std::string digits;
   for (size_t i = p; i < epos; ++i)
      if (s[i] != '.')
         digits += s[i];
   const int exp10 = std::stoi(s.substr(epos + 1));
   const int decpt = exp10 + 1;
   if (neg) out += '-';
   if (decpt > 16 || decpt <= -4) {
      out += digits[0];
      if (digits.size() > 1) {
         out += '.';
         out.append(digits, 1, std::string::npos);
      }
      char e[16];
      std::snprintf(e, sizeof e, "e%c%02d", exp10 < 0 ? '-' : '+', exp10 < 0 ? -exp10 : exp10);
      out += e;
   } else if (decpt <= 0) {
      out += "0.";
      out.append(static_cast<size_t>(-decpt), '0');
      out += digits;
   } else if (static_cast<size_t>(decpt) >= digits.size()) {
      out += digits;
      out.append(static_cast<size_t>(decpt) - digits.size(), '0');
      out += ".0";
   } else {
      out.append(digits, 0, static_cast<size_t>(decpt));
      out += '.';
      out.append(digits, static_cast<size_t>(decpt), std::string::npos);
   }
}

void append_u16(std::string& out, unsigned cu)
{
   char b[8];
   std::snprintf(b, sizeof b, "\\u%04x", cu);
   out += b;
}

// json.dumps(str) with ensure_ascii=True.
void append_string(std::string& out, const std::string& s)
{
   out += '"';
   const size_t n = s.size();
   for (size_t i = 0; i < n;) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c < 0x80) {
         switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
               if (c < 0x20) append_u16(out, c);
               else out += static_cast<char>(c);
         }
         ++i;
         continue;
      }
      int len = 0;
      uint32_t cp = 0;
      if ((c & 0xE0) == 0xC0)      { len = 2; cp = c & 0x1F; }
      else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
      else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
      else throw std::runtime_error("dump_python: invalid UTF-8 in string");
      if (i + len > n)
         throw std::runtime_error("dump_python: truncated UTF-8 in string");
      for (int k = 1; k < len; ++k) {
         const unsigned char cc = static_cast<unsigned char>(s[i + k]);
         if ((cc & 0xC0) != 0x80)
            throw std::runtime_error("dump_python: invalid UTF-8 in string");
         cp = (cp << 6) | (cc & 0x3F);
      }
      static const uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
      if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
         throw std::runtime_error("dump_python: invalid UTF-8 in string");
      if (cp >= 0x10000) {
         cp -= 0x10000;
         append_u16(out, 0xD800 + (cp >> 10));
         append_u16(out, 0xDC00 + (cp & 0x3FF));
      } else {
         append_u16(out, cp);
      }
      i += len;
   }
   out += '"';
}

void dump(std::string& out, const Json& j, int indent, int level)
{
   const bool pretty = indent >= 0;
   auto newline = [&](int lv) {
      out += '\n';
      out.append(static_cast<size_t>(lv) * static_cast<size_t>(indent), ' ');
   };
   switch (j.type()) {
      case Json::value_t::null:            out += "null"; break;
      case Json::value_t::boolean:         out += j.get<bool>() ? "true" : "false"; break;
      case Json::value_t::number_integer:  out += std::to_string(j.get<int64_t>()); break;
      case Json::value_t::number_unsigned: out += std::to_string(j.get<uint64_t>()); break;
      case Json::value_t::number_float:    append_float(out, j.get<double>()); break;
      case Json::value_t::string:          append_string(out, j.get_ref<const std::string&>()); break;
      case Json::value_t::array: {
         if (j.empty()) { out += "[]"; break; }
         out += '[';
         bool first = true;
         for (const auto& e : j) {
            if (!first) out += pretty ? "," : ", ";
            first = false;
            if (pretty) newline(level + 1);
            dump(out, e, indent, level + 1);
         }
         if (pretty) newline(level);
         out += ']';
         break;
      }
      case Json::value_t::object: {
         if (j.empty()) { out += "{}"; break; }
         out += '{';
         bool first = true;
         for (const auto& kv : j.items()) {
            if (!first) out += pretty ? "," : ", ";
            first = false;
            if (pretty) newline(level + 1);
            append_string(out, kv.key());
            out += ": ";
            dump(out, kv.value(), indent, level + 1);
         }
         if (pretty) newline(level);
         out += '}';
         break;
      }
      default:
         throw std::runtime_error("dump_python: unsupported JSON value (binary/discarded)");
   }
}

} // namespace

std::string dump_python(const Json& value, int indent)
{
   std::string out;
   dump(out, value, indent, 0);
   return out;
}

} // namespace ac
