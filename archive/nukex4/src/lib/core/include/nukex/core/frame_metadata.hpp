#pragma once

#include <cstdint>
#include <string>

namespace nukex {

/// Per-frame metadata extracted from FITS headers.
/// Lives in core because multiple libs need it (io, classify, combine).
struct FrameMetadata {
    // Noise/calibration
    float read_noise    = 3.0f;   // electrons (FITS: RDNOISE, READNOISE, NOISE)
    float gain          = 1.0f;   // e-/ADU   (FITS: GAIN, EGAIN)
    float exposure      = 0.0f;   // seconds  (FITS: EXPTIME, EXPOSURE)
    float temperature   = 0.0f;   // Celsius  (FITS: CCD-TEMP, SET-TEMP)

    // Astrometry / pointing
    float plate_scale   = 0.0f;   // arcsec/pixel (FITS: PIXSCALE or derived)
    float focal_length  = 0.0f;   // mm (FITS: FOCALLEN)
    float pixel_size    = 0.0f;   // μm (FITS: XPIXSZ)
    float altitude      = 0.0f;   // degrees above horizon (FITS: OBJCTALT)
    float ra            = 0.0f;   // degrees (FITS: RA)
    float dec           = 0.0f;   // degrees (FITS: DEC)

    // Image dimensions
    int   width         = 0;
    int   height        = 0;
    int   bitpix        = 16;     // FITS BITPIX

    // Filter and camera
    std::string filter;           // FITS: FILTER
    std::string instrument;       // FITS: INSTRUME
    std::string bayer_pattern;    // FITS: BAYERPAT
    std::string date_obs;         // FITS: DATE-OBS

    // Flags
    bool  is_meridian_flipped = false;  // detected during alignment
    bool  has_noise_keywords  = false;  // RDNOISE + GAIN both present
    bool  has_plate_scale     = false;
    bool  has_wcs             = false;  // WCS astrometry headers present

    // Frame index in the session (0-based, arrival order)
    int   frame_index = 0;
};

} // namespace nukex
