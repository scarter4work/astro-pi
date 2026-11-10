// Engine configuration: every field and default of the Python core's config.py (PCL-free).
#pragma once

#include <optional>

namespace gdg {

/// config.Gains. These are the ENGINE defaults (headless script / sidecar); the dialog's are
/// brightness 0.65, size 0.85, contrast 0.12, saturation 0.4 (plan D9).
struct Gains {
    double brightness = 0.5;
    double size = 0.4;
    double contrast = 0.3;
    double saturation = 0.3;
    /// Overall intensity of the synthetic star-shine halo on enlarged stars; independent of
    /// `size` (footprint) so glow can be dialled without re-widening.
    double halo_gain = 1.0;
};

/// config.GradeConfig. `cache_dir` is not here: the online cache (Task 13) owns it.
struct GradeConfig {
    Gains gains;
    double p_low = 5.0;
    double p_high = 95.0;
    /// 4 px absorbs the ~2 px astrometric scatter of a linear WCS export.
    double match_tolerance_px = 4.0;
    double min_match_rate = 0.3;
    double detect_fwhm = 3.0;
    double detect_threshold_sigma = 6.0;
    double base_sigma_px = 2.0;
    double neutral_strength = 0.0;
    /// Drop Gaia sources fainter than this G magnitude; nullopt disables the cut (Python None).
    std::optional<double> gaia_mag_limit = 18.0;
    /// Parallax S/N floor for the offline local-DR3 source (unused online).
    double gaia_parallax_snr = 5.0;
    /// Star-shine halo: at least `halo_min_refs` clean reference stars, else a Moffat of slope
    /// `halo_moffat_beta`; the profile extends to halo_radius_sigma * base_sigma_px.
    int halo_min_refs = 8;
    double halo_radius_sigma = 8.0;
    double halo_moffat_beta = 2.5;
};

}  // namespace gdg
