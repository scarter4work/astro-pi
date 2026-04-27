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

class QEDatabase {
public:
    QEDatabase() = default;

    LoadResult load_shipped(const std::string& path);
    LoadResult load_override(const std::string& path);

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

    LoadResult parse_and_merge(const std::string& path, bool is_override);
};

} // namespace nukex

#endif
