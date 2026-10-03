#ifndef NUKEX_CALIBRATION_QE_DATABASE_HPP
#define NUKEX_CALIBRATION_QE_DATABASE_HPP

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace nukex {

enum class Photosite { R, G, B, MONO_PEAK };

enum class QEConfidence { UNKNOWN, LOW, MEDIUM, HIGH };

struct EmissionLine {
    std::string name;
    double      wavelength_nm = 0.0;
    double      fwhm_nm       = 0.0;
};

struct FilterPassband {
    std::vector<EmissionLine> lines;
    std::string               type;     // "DUAL_NB", "BROADBAND", etc.
};

struct CameraQE {
    std::string                                 sensor;
    std::string                                 type;       // "OSC" / "mono" / "both-variants"
    std::string                                 bayer;      // "RGGB", "BGGR", etc. (empty for mono)
    std::map<int, std::map<Photosite, double>>  qe_by_wavelength;  // sorted by wavelength
    QEConfidence                                confidence = QEConfidence::UNKNOWN;
};

struct LoadResult {
    bool        ok = false;
    std::string error;
};

// Returns the QE database JSON compiled into the module binary (embedded at
// build time from share/qe_database.json). Defined in the generated TU
// qe_database_embedded.cpp -- see embed_qe_database.cmake.
std::string embedded_qe_database_json();

// The db_version the embedded database was published as (the signed
// repository/qe_manifest.json whose db_sha512 matches it), so the updater
// never offers the database the module already carries. 0 when the
// embedded bytes match no publication (an unreleased local edit).
int embedded_qe_database_version();

class QEDatabase {
public:
    QEDatabase() = default;

    // Parse the compiled-in database. This is the shipped baseline: no file,
    // no path lookup, no working-directory assumption -- it cannot be "not
    // found". A newer database downloaded by the updater is loaded with
    // load_shipped(path) instead (see active_qe_database()).
    LoadResult load_embedded();
    // Parse a database from a file on disk (downloaded update, tests).
    LoadResult load_shipped(const std::string& path);
    LoadResult load_override(const std::string& path);

    // Key used for the spec-6.3 unknown-INSTRUME fallback. Shipped by
    // tools/import_qe_research.py as the mean of Sony-sensor OSC cameras.
    static constexpr const char* kGenericOSCCamera = "generic_sony_imx_osc";

    // Lowercase, alphanumerics only. Applied to every camera key on load and
    // to every camera argument on lookup, so "ASI585MC" == "asi585mc".
    static std::string normalize_camera_key(const std::string& raw);

    // Maps a FITS INSTRUME value onto a DB camera key, in three tiers:
    //   1. exact normalised match ("ASI585MC" -> "asi585mc");
    //   2. the longest DB key contained in the normalised INSTRUME
    //      ("ZWO ASI2400MC Pro" -> "asi2400mc");
    //   3. the sensor itself, for rebadged cameras carrying no DB product
    //      key: the trailing number plus the vendor's mono/colour marker
    //      are matched against each entry's `sensor` field and `type`
    //      ("ATR585M" -> IMX585 + mono -> "asi585mm"). Matching uses the
    //      sensor field, never the product key -- ASI2600MM is an IMX571.
    //      A name with no mono/colour marker ("ASI585") stays unresolved
    //      rather than guessing between a sensor's mono and OSC variants,
    //      and kGenericOSCCamera is never returned from this tier.
    // On a tie within a tier the lexicographically smaller key wins, so the
    // result is deterministic regardless of the map's iteration order.
    // Returns "" when nothing matches; callers decide between failing loud
    // and kGenericOSCCamera.
    std::string resolve_camera(const std::string& instrume) const;

    bool has_camera(const std::string& name) const;
    bool has_filter(const std::string& name) const;

    QEConfidence confidence(const std::string& camera) const;

    // Returns 0.0 if camera unknown or wavelength out of bounds.
    // Otherwise: nearest-wavelength QE if outside data range, linear interpolation otherwise.
    double lookup_camera_qe(const std::string& camera,
                            double             wavelength_nm,
                            Photosite          photosite) const;

    FilterPassband lookup_filter(const std::string& name) const;

    int n_cameras() const { return static_cast<int>(cameras_.size()); }
    int n_filters() const { return static_cast<int>(filters_.size()); }

private:
    std::unordered_map<std::string, CameraQE>       cameras_;
    std::unordered_map<std::string, FilterPassband> filters_;

    // (model number, mono/colour) -> camera key, for the third resolution
    // tier. Holds both the vendor product number and the sensor designation
    // of every camera, read straight off the loaded entries: which sensor a
    // product number denotes is a stored fact, never one derived from the
    // digits. Rebuilt after each successful load or override merge.
    std::unordered_map<std::string, std::string>    sensor_index_;

    void rebuild_sensor_index();
    static std::string sensor_index_key(const std::string& number, bool mono);

    LoadResult parse_and_merge(const std::string& text, const char* context);
};

} // namespace nukex

#endif
