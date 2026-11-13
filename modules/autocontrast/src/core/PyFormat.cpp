#include "core/PyFormat.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace ac {

// C's "%.*g" and Python's '.Ng' share the rules that matter here: correct rounding of the exact binary
// value (glibc printf and CPython's dtoa both round correctly, ties-to-even on the exact value), scientific
// form when the exponent is < -4 or >= precision, trailing zeros and a bare '.' removed, exponent sign
// always shown with at least two digits, "inf"/"-inf". They differ only on NaN, where glibc prints "-nan"
// for a negative NaN and Python prints "nan".
std::string py_format_g(double x, int precision)
{
   if (precision < 1)
      throw std::invalid_argument("py_format_g: precision must be >= 1");
   if (std::isnan(x))
      return "nan";
   char buf[64];
   const int n = std::snprintf(buf, sizeof buf, "%.*g", precision, x);
   if (n < 0 || static_cast<size_t>(n) >= sizeof buf)
      throw std::runtime_error("py_format_g: formatting failed");
   return std::string(buf, static_cast<size_t>(n));
}

} // namespace ac
