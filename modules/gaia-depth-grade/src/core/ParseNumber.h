// Portable exact parsing of a complete decimal floating-point number (PCL-free).
#pragma once

#include <string>

namespace gdg {

/// Parses the whole of `text` exactly as std::from_chars(double) does (optional leading '-', no
/// '+', no whitespace; "inf" / "nan" parse as from_chars parses them) into the correctly rounded
/// double, subnormals included. False for anything else, for overflow and for underflow to zero.
///
/// std::from_chars(double) lives in the runtime libstdc++, and the GCC 11 one (Rocky 9) reports
/// errc::result_out_of_range for subnormal results (5e-324), which newer libstdc++ parses. On that
/// error the text is re-parsed with strtod_l under a "C" locale (never the global locale) and a
/// finite non-zero result is accepted. Reentrant; the locale is created once, thread-safely.
bool parse_double_exact(const std::string& text, double& out);

/// The same for float (strtof_l fallback): 1e-45 -> the smallest subnormal float.
bool parse_float_exact(const std::string& text, float& out);

}  // namespace gdg
