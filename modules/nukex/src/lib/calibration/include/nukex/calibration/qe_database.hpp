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
// qe_database_embedded.cpp — see embed_qe_database.cmake.
std::string embedded_qe_database_json();

class QEDatabase {
public:
    QEDatabase() = default;

    // Parse the compiled-in database. This is the production path: no file,
    // no path lookup, no working-directory assumption — it cannot be "not found".
    LoadResult load_embedded();
    // Parse a database from a file on disk (tests / advanced overrides).
    LoadResult load_shipped(const std::string& path);
    LoadResult load_override(const std::string& path);

    // Camera identification.
    //
    // Callers pass whatever the frame says (the raw FITS INSTRUME string,
    // e.g. "ZWO ASI585MC Air"); the DB keys cameras by ids ("asi585mc").
    // A name resolves when its normalized key (see normalize_camera_key)
    // equals the normalized key of a camera id OR of one of that camera's
    // explicit `aliases` in the JSON. There is NO fuzzy matching: no token
    // stripping, no prefix/suffix guessing, no nearest match. A name that is
    // not listed verbatim (modulo case/punctuation) stays unknown, so an
    // unlisted camera fails loudly instead of borrowing a wrong QE curve
    // (e.g. mono "ASI585MM" can never land on colour "asi585mc").
    //
    // resolve_camera_id returns the DB id, or "" when nothing matches.
    std::string resolve_camera_id(const std::string& name) const;

    // Lowercase ASCII alphanumerics only: "ZWO ASI585MC Air" -> "zwoasi585mcair".
    static std::string normalize_camera_key(const std::string& name);

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
    // normalize_camera_key(id or alias) -> camera id (key of cameras_).
    std::unordered_map<std::string, std::string>    camera_keys_;

    LoadResult parse_and_merge(const std::string& text, const char* context);
    const CameraQE* find_camera(const std::string& name) const;
};

} // namespace nukex

#endif
