#include "nukex/io/fits_reader.hpp"
#include <fitsio.h>
#include <cstring>
#include <cmath>
#include <vector>

namespace nukex {

float FITSReader::read_float_key(void* fptr, const char* key, float default_val) {
    float value = default_val;
    int status = 0;
    fits_read_key(static_cast<fitsfile*>(fptr), TFLOAT, key, &value, nullptr, &status);
    if (status != 0) return default_val;
    return value;
}

int FITSReader::read_int_key(void* fptr, const char* key, int default_val) {
    int value = default_val;
    int status = 0;
    fits_read_key(static_cast<fitsfile*>(fptr), TINT, key, &value, nullptr, &status);
    if (status != 0) return default_val;
    return value;
}

std::string FITSReader::read_string_key(void* fptr, const char* key) {
    char value[FLEN_VALUE] = {};
    int status = 0;
    fits_read_key(static_cast<fitsfile*>(fptr), TSTRING, key, value, nullptr, &status);
    if (status != 0) return "";
    // Trim trailing spaces
    int len = static_cast<int>(strlen(value));
    while (len > 0 && value[len - 1] == ' ') len--;
    return std::string(value, len);
}

FrameMetadata FITSReader::extract_metadata(void* fptr) {
    FrameMetadata m;
    fitsfile* f = static_cast<fitsfile*>(fptr);

    // Dimensions — reset status before each CFITSIO call block so a failure
    // in one doesn't silently cause subsequent calls to no-op.
    int naxis = 0;
    long naxes[3] = {0, 0, 0};
    int status = 0;
    fits_get_img_dim(f, &naxis, &status);
    if (status != 0) {
        // Could not read image dimensions; reset and continue with defaults
        status = 0;
    }
    fits_get_img_size(f, 3, naxes, &status);
    if (status != 0) {
        status = 0;
    }
    m.width  = static_cast<int>(naxes[0]);
    m.height = static_cast<int>(naxes[1]);
    m.bitpix = read_int_key(f, "BITPIX", 16);

    // Exposure
    m.exposure = read_float_key(f, "EXPTIME", 0.0f);
    if (m.exposure == 0.0f) m.exposure = read_float_key(f, "EXPOSURE", 0.0f);

    // Gain — try EGAIN first (e-/ADU), then GAIN
    float egain = read_float_key(f, "EGAIN", -1.0f);
    if (egain > 0.0f) {
        m.gain = egain;
    } else {
        m.gain = read_float_key(f, "GAIN", 1.0f);
    }

    // Read noise — try multiple keywords
    float rdnoise = read_float_key(f, "RDNOISE", -1.0f);
    if (rdnoise < 0.0f) rdnoise = read_float_key(f, "READNOISE", -1.0f);
    if (rdnoise < 0.0f) rdnoise = read_float_key(f, "NOISE", -1.0f);
    if (rdnoise > 0.0f) {
        m.read_noise = rdnoise;
        m.has_noise_keywords = (m.gain > 0.0f);
    }

    // Temperature
    m.temperature = read_float_key(f, "CCD-TEMP", 0.0f);
    if (m.temperature == 0.0f) m.temperature = read_float_key(f, "SET-TEMP", 0.0f);

    // Plate scale
    m.focal_length = read_float_key(f, "FOCALLEN", 0.0f);
    m.pixel_size   = read_float_key(f, "XPIXSZ", 0.0f);
    float pixscale = read_float_key(f, "PIXSCALE", 0.0f);
    if (pixscale > 0.0f) {
        m.plate_scale = pixscale;
        m.has_plate_scale = true;
    } else if (m.focal_length > 0.0f && m.pixel_size > 0.0f) {
        m.plate_scale = 206.265f * m.pixel_size / m.focal_length;
        m.has_plate_scale = true;
    }

    // Coordinates
    m.ra  = read_float_key(f, "RA", 0.0f);
    m.dec = read_float_key(f, "DEC", 0.0f);
    m.altitude = read_float_key(f, "OBJCTALT", 0.0f);

    // Strings
    m.filter        = read_string_key(f, "FILTER");
    m.instrument    = read_string_key(f, "INSTRUME");
    m.bayer_pattern = read_string_key(f, "BAYERPAT");
    m.date_obs      = read_string_key(f, "DATE-OBS");

    // WCS detection
    std::string ctype1 = read_string_key(f, "CTYPE1");
    m.has_wcs = !ctype1.empty();

    return m;
}

FITSReadResult FITSReader::read(const std::string& filepath) {
    FITSReadResult result;
    fitsfile* fptr = nullptr;
    int status = 0;

    // Open FITS file
    fits_open_file(&fptr, filepath.c_str(), READONLY, &status);
    if (status != 0) {
        char err[FLEN_ERRMSG];
        fits_get_errstatus(status, err);
        result.error = std::string("Failed to open FITS: ") + err;
        return result;
    }

    // Extract metadata
    result.metadata = extract_metadata(fptr);

    // Get image parameters
    int naxis = 0;
    long naxes[3] = {0, 0, 0};
    fits_get_img_dim(fptr, &naxis, &status);
    fits_get_img_size(fptr, 3, naxes, &status);

    int width  = static_cast<int>(naxes[0]);
    int height = static_cast<int>(naxes[1]);
    int n_channels = (naxis >= 3) ? static_cast<int>(naxes[2]) : 1;

    // Allocate image
    result.image = Image(width, height, n_channels);

    // Read pixel data as float, channel by channel
    long fpixel[3] = {1, 1, 1};  // CFITSIO uses 1-based indexing
    int anynul = 0;
    float nullval = 0.0f;

    for (int ch = 0; ch < n_channels; ch++) {
        fpixel[2] = ch + 1;
        long n_pixels = static_cast<long>(width) * height;

        fits_read_pix(fptr, TFLOAT, fpixel, n_pixels,
                      &nullval, result.image.channel_data(ch),
                      &anynul, &status);
        if (status != 0) {
            char err[FLEN_ERRMSG];
            fits_get_errstatus(status, err);
            result.error = std::string("Failed to read pixels: ") + err;
            fits_close_file(fptr, &status);
            return result;
        }
    }

    // Normalize pixel data to [0, 1]
    int bitpix = 0;
    int bitpix_status = 0;
    fits_get_img_type(fptr, &bitpix, &bitpix_status);
    if (bitpix_status == 0 &&
        (bitpix == BYTE_IMG || bitpix == SHORT_IMG || bitpix == LONG_IMG ||
         bitpix == USHORT_IMG || bitpix == ULONG_IMG)) {
        // CFITSIO applies BZERO/BSCALE automatically when reading as TFLOAT,
        // so 16-bit data with BZERO=32768 comes back as unsigned range [0, 65535].
        // Normalize to [0, 1].
        float max_val = 0.0f;
        const float* d = result.image.data();
        for (size_t i = 0; i < result.image.data_size(); i++) {
            if (d[i] > max_val) max_val = d[i];
        }
        if (max_val > 0.0f) {
            float scale = 1.0f / max_val;
            result.image.apply([scale](float x) { return x * scale; });
        }
    } else if (bitpix_status == 0 &&
               (bitpix == FLOAT_IMG || bitpix == DOUBLE_IMG)) {
        // Float FITS files may contain raw ADU values (e.g. 0-65535) rather
        // than normalized [0,1]. Scan for max and normalize if clearly not
        // already in [0,1] range.
        float max_val = 0.0f;
        const float* d = result.image.data();
        for (size_t i = 0; i < result.image.data_size(); i++) {
            if (d[i] > max_val) max_val = d[i];
        }
        if (max_val > 1.5f) {
            float scale = 1.0f / max_val;
            result.image.apply([scale](float x) { return x * scale; });
        }
    }

    fits_close_file(fptr, &status);
    result.success = true;
    return result;
}

FrameMetadata FITSReader::read_headers(const std::string& filepath) {
    FrameMetadata m;
    fitsfile* fptr = nullptr;
    int status = 0;

    fits_open_file(&fptr, filepath.c_str(), READONLY, &status);
    if (status != 0) return m;

    m = extract_metadata(fptr);
    fits_close_file(fptr, &status);
    return m;
}

} // namespace nukex
