// Text handling for the ESA Gaia TAP service (plan Task 13): the UWS job location, the CSV
// result, and the per-field cache key (port of GaiaStarSource._cache_path).
#pragma once

#include "Catalog.h"
#include "Log.h"

#include <optional>
#include <string>
#include <vector>

namespace gdg {

/// The only host and service the online source talks to.
inline constexpr const char* kGaiaArchive = "https://gea.esac.esa.int/";
inline constexpr const char* kGaiaTapAsync = "https://gea.esac.esa.int/tap-server/tap/async";
inline constexpr const char* kGaiaNotices = "https://gea.esac.esa.int/tap-server/notification?action=GetNotifications";

/// The job URL from the body of the 303 answer to an async job submission: a line
/// `Location: https://gea.esac.esa.int/tap-server/tap/async/<id>`. Throws std::runtime_error
/// when there is no such line, or when it points anywhere but that service (another host,
/// scheme, path, or extra path/query).
std::string parse_uws_location(const std::string& body);

/// Data rows of a CSV text (non-empty lines after the header).
long csv_row_count(const std::string& csv);

/// The result CSV (columns ra, dec, r_med_geo, r_lo_geo, r_hi_geo, source_id [, bp_rp], any
/// order, others ignored) as a catalogue. ra/dec are DOUBLE in the archive, the distances and
/// bp_rp REAL (float32, printed with float precision): they are parsed as float and widened, the
/// values the Python core got from the VOTable. An empty field is NaN. Throws std::runtime_error
/// `Gaia result missing columns: ['a', 'b']` (Python's sorted list repr) or
/// `Gaia query returned zero rows for this field`.
Catalog parse_tap_csv(const std::string& csv, const Log& log);

/// One row of the plate solver's online catalogue (build_positions_adql).
struct GaiaPosition {
    double ra = 0, dec = 0;      // degrees, ICRS, epoch J2016.0
    double pmra = 0, pmdec = 0;  // mas/yr (pmra includes cos dec)
    double parallax = 0;         // mas; 0 when the archive has none
    double g = 0;                // G magnitude
};

/// The positions CSV (columns ra, dec, pmra, pmdec, parallax, phot_g_mean_mag, any order). ra,
/// dec, pmra, pmdec and parallax are DOUBLE in the archive, phot_g_mean_mag REAL. Rows lacking a
/// position, proper motion or G are skipped; a missing parallax is 0. Zero rows is an empty
/// result (the solver then reports too few catalogue stars). Throws std::runtime_error
/// `Gaia result missing columns: [...]`.
std::vector<GaiaPosition> parse_positions_csv(const std::string& csv);

/// SHA-1 of a byte string, lowercase hex (FIPS 180-4; for the cache file name only).
std::string sha1_hex(const std::string& bytes);

/// GaiaStarSource._cache_path's digest: the first 16 hex characters of SHA-1 over
/// f"{ra:.6f}_{dec:.6f}_{radius:.6f}_mag{mag_limit}" (mag_limit as Python repr, or None).
std::string cache_key(const Footprint& fp, std::optional<double> mag_limit);

}  // namespace gdg
