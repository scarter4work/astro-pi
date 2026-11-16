#include "nukex/io/fits_reader.hpp"
#include <cctype>
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

bool FITSReader::try_read_float_key(void* fptr, const char* key, float& out) {
    float value = 0.0f;
    int status = 0;
    fits_read_key(static_cast<fitsfile*>(fptr), TFLOAT, key, &value, nullptr, &status);
    if (status != 0) return false;
    out = value;
    return true;
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

    // Gain — EGAIN is electronic gain in e-/ADU by convention. GAIN usually
    // is NOT: on ZWO and QHY cameras it is the gain MENU INDEX (0-500), so
    // reading it as e-/ADU puts a number like 200 into the Poisson term and
    // understates shot noise by orders of magnitude. Older CCD software does
    // write GAIN in e-/ADU, so it is still accepted when the value is one a
    // real detector could have; above that ceiling it is an index, and the
    // honest answer is that the gain is unknown -- which leaves
    // has_noise_keywords false and sends Phase B to the across-frame scale.
    constexpr float MAX_PLAUSIBLE_EGAIN = 25.0f;   // e-/ADU
    // Camera identity settles what GAIN means. ZWO (ASI) and QHY firmware
    // write the gain MENU INDEX, and a low index (0-25) is a legitimate
    // setting on those cameras that the plausibility ceiling alone would
    // accept as e-/ADU -- a 10x error in the Poisson term.
    m.instrument = read_string_key(f, "INSTRUME");
    std::string inst_upper = m.instrument;
    for (auto& c : inst_upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const bool gain_is_menu_index =
        inst_upper.find("ZWO") != std::string::npos ||
        inst_upper.find("ASI") != std::string::npos ||
        inst_upper.find("QHY") != std::string::npos;
    bool gain_known = false;
    float egain = read_float_key(f, "EGAIN", -1.0f);
    if (egain > 0.0f) {
        m.gain = egain;
        gain_known = true;
    } else {
        const float g = read_float_key(f, "GAIN", -1.0f);
        if (!gain_is_menu_index && g > 0.0f && g <= MAX_PLAUSIBLE_EGAIN) {
            m.gain = g;
            gain_known = true;
        } else {
            m.gain = 1.0f;      // unknown; unused while has_noise_keywords is false
        }
    }

    // Read noise — try multiple keywords
    float rdnoise = read_float_key(f, "RDNOISE", -1.0f);
    if (rdnoise < 0.0f) rdnoise = read_float_key(f, "READNOISE", -1.0f);
    if (rdnoise < 0.0f) rdnoise = read_float_key(f, "NOISE", -1.0f);
    if (rdnoise > 0.0f) {
        m.read_noise = rdnoise;
        m.has_noise_keywords = gain_known;
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
    m.bayer_pattern = read_string_key(f, "BAYERPAT");
    m.date_obs      = read_string_key(f, "DATE-OBS");

    // WCS detection
    std::string ctype1 = read_string_key(f, "CTYPE1");
    m.has_wcs = !ctype1.empty();

    // Calibration matching: raw values with presence flags (see FrameMetadata).
    m.image_type = read_string_key(f, "IMAGETYP");
    if (m.image_type.empty()) m.image_type = read_string_key(f, "FRAMETYP");
    if (m.image_type.empty()) m.image_type = read_string_key(f, "FRAME");
    m.has_gain_setting = try_read_float_key(f, "GAIN", m.gain_setting);
    m.has_offset = try_read_float_key(f, "OFFSET", m.offset);
    if (!m.has_offset) m.has_offset = try_read_float_key(f, "BLKLEVEL", m.offset);
    m.has_ccd_temp = try_read_float_key(f, "CCD-TEMP", m.ccd_temp);
    m.has_set_temp = try_read_float_key(f, "SET-TEMP", m.set_temp);
    {
        float t = 0.0f;
        m.has_exposure = try_read_float_key(f, "EXPTIME", t) ||
                         try_read_float_key(f, "EXPOSURE", t);
    }
    m.xbinning = read_int_key(f, "XBINNING", 1);
    m.ybinning = read_int_key(f, "YBINNING", 1);
    {
        int eq = 0, eq_status = 0;
        fits_get_img_equivtype(f, &eq, &eq_status);
        m.equiv_bitpix = (eq_status == 0) ? eq : 0;
    }

    return m;
}

FITSReadResult FITSReader::read(const std::string& filepath) {
    return read_impl(filepath, Scaling::PER_FRAME_MAX);
}

FITSReadResult FITSReader::read_fixed_scale(const std::string& filepath) {
    return read_impl(filepath, Scaling::FIXED);
}

FITSReadResult FITSReader::read_raw(const std::string& filepath) {
    return read_impl(filepath, Scaling::NONE);
}

float FITSReader::full_scale_for_bitpix(int equiv_bitpix) {
    switch (equiv_bitpix) {
        case BYTE_IMG:   return 255.0f;
        case SBYTE_IMG:  return 127.0f;
        case USHORT_IMG: return 65535.0f;
        case SHORT_IMG:  return 32767.0f;
        case ULONG_IMG:  return 4294967295.0f;
        case LONG_IMG:   return 2147483647.0f;
        default:         return 0.0f;   // float types: decided by the data
    }
}

FITSReadResult FITSReader::read_impl(const std::string& filepath, Scaling scaling) {
    FITSReadResult result;
    fitsfile* fptr = nullptr;
    int status = 0;

    // Open FITS file. A frame path is a local file path: fits_open_diskfile()
    // does no filename parsing, so http://, ftp:// and root:// names are never
    // handed to CFITSIO's network drivers (fits_open_file() would connect).
    fits_open_diskfile(&fptr, filepath.c_str(), READONLY, &status);
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

    if (scaling == Scaling::NONE) {
        result.full_scale = 1.0f;
        fits_close_file(fptr, &status);
        result.success = true;
        return result;
    }
    if (scaling == Scaling::FIXED) {
        // One scale per data type, never per frame (see read_fixed_scale).
        float fs = full_scale_for_bitpix(result.metadata.equiv_bitpix);
        if (fs == 0.0f) {
            float max_val = 0.0f;
            const float* d = result.image.data();
            for (size_t i = 0; i < result.image.data_size(); i++) {
                if (d[i] > max_val) max_val = d[i];
            }
            fs = (max_val > 1.5f) ? 65535.0f : 1.0f;
        }
        if (fs != 1.0f) {
            const float scale = 1.0f / fs;
            result.image.apply([scale](float x) { return x * scale; });
        }
        result.full_scale = fs;
        fits_close_file(fptr, &status);
        result.success = true;
        return result;
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

    fits_open_diskfile(&fptr, filepath.c_str(), READONLY, &status);
    if (status != 0) return m;

    m = extract_metadata(fptr);
    fits_close_file(fptr, &status);
    return m;
}

} // namespace nukex

namespace nukex {

namespace {
// A card's value as written: quotes removed from a string ('' -> '),
// surrounding blanks trimmed.
std::string card_value(const char* raw) {
    std::string v(raw);
    auto trim = [](std::string& t) {
        while (!t.empty() && t.back() == ' ') t.pop_back();
        std::size_t i = 0;
        while (i < t.size() && t[i] == ' ') ++i;
        t.erase(0, i);
    };
    trim(v);
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') {
        std::string inner;
        for (std::size_t i = 1; i + 1 < v.size(); ++i) {
            inner.push_back(v[i]);
            if (v[i] == '\'' && i + 2 < v.size() && v[i + 1] == '\'') ++i;
        }
        trim(inner);
        return inner;
    }
    return v;
}
} // namespace

std::map<std::string, std::string> FITSReader::read_keywords(const std::string& filepath) {
    std::map<std::string, std::string> out;
    fitsfile* fptr = nullptr;
    int status = 0;
    fits_open_diskfile(&fptr, filepath.c_str(), READONLY, &status);
    if (status != 0) return out;
    int nkeys = 0;
    fits_get_hdrspace(fptr, &nkeys, nullptr, &status);
    for (int i = 1; status == 0 && i <= nkeys; ++i) {
        char name[FLEN_KEYWORD] = {}, value[FLEN_VALUE] = {}, comment[FLEN_COMMENT] = {};
        fits_read_keyn(fptr, i, name, value, comment, &status);
        if (status != 0) break;
        const std::string key(name);
        if (key.empty() || key == "COMMENT" || key == "HISTORY") continue;
        out[key] = card_value(value);
    }
    int close_status = 0;
    fits_close_file(fptr, &close_status);
    return out;
}

std::vector<std::string> FITSReader::read_history(const std::string& filepath) {
    std::vector<std::string> out;
    fitsfile* fptr = nullptr;
    int status = 0;
    fits_open_diskfile(&fptr, filepath.c_str(), READONLY, &status);
    if (status != 0) return out;
    int nkeys = 0;
    fits_get_hdrspace(fptr, &nkeys, nullptr, &status);
    for (int i = 1; status == 0 && i <= nkeys; ++i) {
        char card[FLEN_CARD] = {};
        fits_read_record(fptr, i, card, &status);
        if (status != 0) break;
        if (std::strncmp(card, "HISTORY", 7) == 0) {
            std::string text(card + 7);
            while (!text.empty() && text.back() == ' ') text.pop_back();
            if (!text.empty() && text.front() == ' ') text.erase(0, 1);
            out.push_back(text);
        }
    }
    int close_status = 0;
    fits_close_file(fptr, &close_status);
    return out;
}

} // namespace nukex
