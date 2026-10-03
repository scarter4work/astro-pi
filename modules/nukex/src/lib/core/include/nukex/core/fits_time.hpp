#pragma once

#include <string>

namespace nukex {

/// Seconds since 1970-01-01T00:00:00 for a FITS DATE-OBS string
/// ("YYYY-MM-DDTHH:MM:SS[.sss]", a space accepted for the 'T', an optional
/// trailing 'Z'). Returns 0 when the string is absent or unparsable: only
/// differences between frames are ever used, and 0 means "unknown".
double parse_fits_datetime(const std::string& date_obs);

} // namespace nukex
