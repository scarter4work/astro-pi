// AutoContrast core (PCL-free): Python float formatting used in user-visible strings.
#pragma once

#include <string>

namespace ac {

// format(x, f'.{precision}g') in CPython: correctly rounded, trailing zeros stripped, exponent with at
// least two digits ("1e+03"), "inf" / "-inf", and "nan" for every NaN (no sign). precision >= 1.
std::string py_format_g(double x, int precision);

} // namespace ac
