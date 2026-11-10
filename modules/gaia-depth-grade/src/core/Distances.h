// Offline distances from the local Gaia DR3 database (port of gaia_depth_grade/local_gaia.py),
// plus the PixInsight settings-file discovery of the database files (port of the PJSR
// discoverGaiaDbPaths). PCL-free: the module's LocalGaiaSource feeds it GaiaDatabaseFile rows.
#pragma once

#include "Catalog.h"
#include "Log.h"

#include <optional>
#include <string>
#include <vector>

namespace gdg {

/// Median Gaia DR3 parallax uncertainty (mas) for magnitude G: np.interp over the knot table of
/// local_gaia.py (clamped outside 6..21). NaN for NaN.
double sigma_parallax_from_g(double g);
std::vector<double> sigma_parallax_from_g(const std::vector<double>& g);

/// local_gaia.parallax_to_distance_table: distance = 1000/parallax with bounds from the
/// synthesised sigma; sources with a non-finite or non-positive parallax, or S/N < snr_min, are
/// dropped with the warning `local Gaia: dropped %d/%d sources failing the parallax S/N >= %.1f
/// cut (non-positive or low-S/N parallax — distance unreliable). Kept %d.` source_id is the row
/// index among the kept rows; bp_rp passes through (NaN where unknown). All inputs must have the
/// same length (bp_rp may be empty: all NaN).
Catalog parallax_to_distance_table(const std::vector<double>& ra, const std::vector<double>& dec,
                                   const std::vector<double>& parallax_mas, const std::vector<double>& g_mag,
                                   const std::vector<double>& bp_rp, double snr_min, const Log& log);

/// LocalGaiaSource.distances_for after the cone search: the optional strict magnitude cut
/// (G < mag_limit; warning `local Gaia: magnitude cut G<%.1f excludes %d source(s).`), the
/// distance table, the zero-usable error (std::runtime_error `local Gaia produced zero usable
/// sources after the parallax/magnitude cuts for this field`) and the info line
/// `local Gaia: %d usable sources (parallax distances)`.
Catalog local_gaia_distances(const std::vector<double>& ra, const std::vector<double>& dec,
                             const std::vector<double>& parallax_mas, const std::vector<double>& g_mag,
                             const std::vector<double>& bp_rp, std::optional<double> mag_limit, double snr_min,
                             const Log& log);

/// The .xpsd paths a PixInsight core settings file (core-NNN-pxi.settings text) configures for
/// the Gaia process: the DR3DatabaseFilePathNN values when there are any, else the
/// DR3SPDatabaseFilePathNN values, else none (discoverGaiaDbPaths for one file). XML entities in
/// the values are decoded.
std::vector<std::string> gaia_db_paths_from_settings(const std::string& settings_text);

}  // namespace gdg
