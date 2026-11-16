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

    // ── Calibration matching ─────────────────────────────────────────
    // The header values a bias, dark or flat has to share with the light it
    // calibrates, kept AS WRITTEN with a presence flag each. The fields above
    // cannot serve: `gain` is e-/ADU (1.0 when the camera writes a menu
    // index), and `temperature` and `exposure` use 0.0 for "absent", which is
    // also a real reading. Nothing outside dark/bias calibration reads these.
    std::string image_type;           // IMAGETYP (else FRAMETYP, FRAME)
    float gain_setting      = 0.0f;   // GAIN as written (a menu index on ZWO/QHY)
    bool  has_gain_setting  = false;
    float offset            = 0.0f;   // OFFSET (else BLKLEVEL)
    bool  has_offset        = false;
    float ccd_temp          = 0.0f;   // CCD-TEMP: the sensor's measured temperature
    bool  has_ccd_temp      = false;
    float set_temp          = 0.0f;   // SET-TEMP: the cooler's set point
    bool  has_set_temp      = false;
    bool  has_exposure      = false;  // EXPTIME or EXPOSURE present
    int   xbinning          = 1;      // XBINNING (1 when absent)
    int   ybinning          = 1;      // YBINNING (1 when absent)
    /// The data's equivalent BITPIX after BZERO/BSCALE (CFITSIO's
    /// fits_get_img_equivtype): 16-bit camera data with BZERO=32768 reports
    /// 20 (USHORT_IMG), not 16. 0 when it could not be read.
    int   equiv_bitpix      = 0;
};

} // namespace nukex
