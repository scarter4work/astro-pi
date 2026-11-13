// Shared types of the nebula 3D depth reconstruction: a port of recon/model.py (PCL-free).
//
// Conventions (model.py):
// - 2-D planes are mono gdg::Image (channels == 1) of shape (ny, nx) = (height, width), holding
//   float32-valued doubles wherever Python stores float32 (every array output of recon/*).
// - `emissivity` is (ny, nx, 3), linear.
// - `dust` is (ny, nx), opacity in [0, 1].
// - `z` is pixel-relative depth: observer at -inf, 0 = nearest, 1 = farthest.
// - Pixel coordinates are the Python core's: x = column, y = row, row 0 = PixInsight's top row
//   (frame contract C1: PixInsight writes FITS TOP-DOWN, so astropy's data[0] is the top row).
#pragma once

#include "core/Image.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gdg::recon {

/// numpy's float32 cast (`.astype(np.float32)`): round to nearest float, kept as a double.
inline double f32(double v) { return static_cast<double>(static_cast<float>(v)); }

/// np.nan_to_num on float64: NaN -> 0, +inf -> DBL_MAX, -inf -> -DBL_MAX.
inline double nan_to_num(double v) {
    if (std::isnan(v)) return 0.0;
    if (std::isinf(v)) return v > 0 ? 1.7976931348623157e308 : -1.7976931348623157e308;
    return v;
}

/// np.clip(v, lo, hi) for non-NaN v (numpy's max-then-min; only the sign of a zero can differ).
inline double clip(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

/// A boolean (ny, nx) array, row-major like gdg::Image (1 = True).
struct Mask {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> data;

    Mask() = default;
    Mask(int w, int h, bool fill = false)
        : width(w), height(h), data(static_cast<size_t>(w) * static_cast<size_t>(h), fill ? 1 : 0) {}
    bool any() const {
        for (unsigned char v : data)
            if (v) return true;
        return false;
    }
    size_t count() const {
        size_t n = 0;
        for (unsigned char v : data) n += v ? 1 : 0;
        return n;
    }
};

/// model.validate_shapes(*arrays): std::invalid_argument (Python ValueError) unless every
/// non-null array has the same leading (ny, nx) shape. Channel count is not compared (shape[:2]).
inline void validate_shapes(std::initializer_list<const Image*> arrays) {
    std::vector<std::pair<int, int>> shapes;
    for (const Image* a : arrays)
        if (a) shapes.emplace_back(a->height, a->width);
    for (const auto& s : shapes) {
        if (s != shapes.front()) {
            std::string msg = "array shape mismatch: [";
            for (size_t i = 0; i < shapes.size(); ++i) {
                if (i) msg += ", ";
                msg += "(" + std::to_string(shapes[i].first) + ", " + std::to_string(shapes[i].second) + ")";
            }
            throw std::invalid_argument(msg + "]");
        }
    }
}

/// model.ChannelSet. `sii` is optional (duoband/OSC may have none). Construction validates shapes.
struct ChannelSet {
    Image ha;
    Image oiii;
    std::optional<Image> sii;
    double ratio_confidence = 0.0;
    bool is_linear = false;

    ChannelSet(Image ha_, Image oiii_, std::optional<Image> sii_, double ratio_confidence_, bool is_linear_)
        : ha(std::move(ha_)), oiii(std::move(oiii_)), sii(std::move(sii_)),
          ratio_confidence(ratio_confidence_), is_linear(is_linear_) {
        validate_shapes({&ha, &oiii, sii ? &*sii : nullptr});
    }
};

/// model.IonizationMap: (ny, nx) OIII fraction in [0, 1] and the ratio confidence.
struct IonizationMap {
    Image oiii_fraction;
    double confidence = 0.0;
};

/// model.Ionizer. `source` is one of "gaia-blue", "gaia-bright", "field-center", "override".
struct Ionizer {
    double x = 0.0;
    double y = 0.0;
    std::optional<double> distance_pc;
    std::string source;
};

/// model.ReconConfig, every default verbatim.
struct ReconConfig {
    // --- depth-blend weights (reconstruct_cavity) ---
    double w_geometry = 1.0;        ///< weight on geometry_prior (always on)
    double w_ionization = 0.6;      ///< MAXIMUM weight on ionization_depth; used as w * confidence
    double w_emission = 0.15;       ///< gain on the signed emission_measure_relief
    double emission_sign = 1.0;     ///< +1: a bright limb ridge bulges toward the observer
    double w_dust = 0.8;            ///< opacity-1 dust scales depth to (1 - w_dust)
    double footprint_frac = 0.05;   ///< footprint = lum > floor + frac * (peak - floor)
    double geometry_power = 2.0;    ///< paraboloid steepness for geometry_prior
    double ionization_gamma = 1.0;  ///< shaping exponent for ionization_depth
    // --- 2D render parameters (render_cavity_2d, Render2D.h; not part of the cavity meta) ---
    double render_gamma = 1.0;          ///< exponent on the depth shading only
    double render_ambient = 0.05;       ///< shading floor: the farthest depth keeps this fraction
    double render_depth_shading = 0.5;  ///< shading loss per unit of relative depth
    double render_dust_strength = 1.0;  ///< opacity scale of the near-side dust occlusion
    double render_limb_gain = 0.5;      ///< limb brightening at the steepest depth gradient
};

/// reconstruct_cavity's meta dict. Field names are the Python keys verbatim; cavity_meta_json()
/// (Cavity.h) writes them in the Python dict's insertion order.
struct CavityMeta {
    double w_geometry = 0.0;
    double w_ionization_max = 0.0;
    double w_ionization_effective = 0.0;
    double w_emission = 0.0;
    double emission_sign = 0.0;
    double w_dust = 0.0;
    double ionization_confidence = 0.0;
    double ratio_confidence = 0.0;
    bool is_linear = false;
    bool sii_present = false;
    std::string capture_mode;
    std::string ionizer_source;
    double footprint_frac = 0.0;
    long long footprint_px = 0;
};

/// model.CavityModel. Construction validates that z, emissivity and dust share (ny, nx).
struct CavityModel {
    Image z;           ///< (ny, nx) pixel-relative depth
    Image emissivity;  ///< (ny, nx, 3) linear
    Image dust;        ///< (ny, nx) opacity in [0, 1]
    std::optional<double> pc_per_px;
    std::pair<double, double> apex_xy;
    CavityMeta meta;

    CavityModel(Image z_, Image emissivity_, Image dust_, std::optional<double> pc_per_px_,
                std::pair<double, double> apex_xy_, CavityMeta meta_)
        : z(std::move(z_)), emissivity(std::move(emissivity_)), dust(std::move(dust_)),
          pc_per_px(pc_per_px_), apex_xy(apex_xy_), meta(std::move(meta_)) {
        validate_shapes({&z, &emissivity, &dust});
    }
};

}  // namespace gdg::recon
