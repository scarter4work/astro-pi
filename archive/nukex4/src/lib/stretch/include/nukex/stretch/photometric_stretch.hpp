#pragma once
#include "nukex/stretch/stretch_op.hpp"

namespace nukex {

/// Photometric Stretch — maps pixel values to surface brightness (mag/arcsec²).
///
/// Converts raw ADU values to physical flux using detector parameters,
/// applies the logarithmic magnitude scale, and maps a chosen surface
/// brightness range to [0, 1] for display.
///
/// When FITS calibration data is unavailable, falls back to a relative
/// photometric stretch using image statistics as a proxy for zero point.
class PhotometricStretch : public StretchOp {
public:
    // Detector parameters (from FITS headers or user input)
    float gain      = 1.0f;    // e-/ADU
    float exptime   = 1.0f;    // seconds (default 1 for normalized data)
    float zp        = 18.0f;   // Photometric zero point (mag). Default set so that
                               // x=1.0 maps to mu_bright on normalized [0,1] data.
    float plate_scale = 1.0f;  // arcsec/pixel

    // Display range in mag/arcsec²
    float mu_bright = 18.0f;   // Bright cutoff (x=1.0 → 1.0 display)
    float mu_faint  = 28.0f;   // Faint cutoff  (x≈0.0001 → 0.0 display)

    // Sky background level (in normalized [0,1] units).
    // If <= 0, auto-estimated from image median.
    float sky_level = -1.0f;

    bool luminance_only = true;

    PhotometricStretch() { name = "Photometric"; category = StretchCategory::PRIMARY; }

    void  apply(Image& img) const override;

    /// Convert a normalized pixel value to surface brightness,
    /// then map to display range [0, 1].
    float apply_scalar(float x) const override;
};

} // namespace nukex
