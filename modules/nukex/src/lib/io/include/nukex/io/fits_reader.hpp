#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/frame_metadata.hpp"
#include <map>
#include <string>
#include <vector>

namespace nukex {

/// Result of reading a FITS file.
struct FITSReadResult {
    Image         image;     // pixel data as float32
    FrameMetadata metadata;  // extracted header information
    bool          success = false;
    std::string   error;     // error message if !success

    /// The divisor that put the pixels on [0, 1]. Set only by
    /// read_fixed_scale(); read() divides by each frame's own maximum and
    /// leaves this 0.
    float         full_scale = 0.0f;
};

/// Read a FITS file into an Image + FrameMetadata.
///
/// Handles 8/16/32-bit integer and 32/64-bit float FITS.
/// Raw Bayer data is returned as single-channel (debayer happens separately).
/// Pixel values are normalized to [0, 1] for integer types.
///
/// Uses CFITSIO directly (not PCL) for standalone testability.
class FITSReader {
public:
    /// Read a single FITS file.
    static FITSReadResult read(const std::string& filepath);

    /// Read a FITS file on a FIXED scale: integer data is divided by the full
    /// scale of its type (255 for 8-bit, 65535 for unsigned 16-bit, ...), so
    /// every frame of a camera lands on the same scale and stays linear.
    ///
    /// This is what dark and bias calibration needs. read() divides each
    /// frame by its own maximum, which puts a bias (maximum: a hot pixel) and
    /// a light (maximum: a saturated star) on scales that differ by an
    /// unknown factor, and a difference of the two would mean nothing.
    ///
    /// Float data that is already on [0, 1] (maximum <= 1.5, the same test
    /// read() uses) keeps a full scale of 1; float data holding raw ADU is
    /// divided by 65535. `full_scale` reports the divisor.
    static FITSReadResult read_fixed_scale(const std::string& filepath);

    /// Read the pixel values exactly as stored (after BZERO/BSCALE), with no
    /// normalisation at all; `full_scale` is 1. For files whose values are
    /// already on the scale wanted -- a master calibration frame written by
    /// NukeX, which read_fixed_scale() could misjudge if a pixel exceeded 1.5.
    static FITSReadResult read_raw(const std::string& filepath);

    /// The fixed full scale read_fixed_scale() uses for an equivalent BITPIX
    /// (FrameMetadata::equiv_bitpix). 0 for float types, whose scale depends
    /// on the data, and for an unknown type.
    static float full_scale_for_bitpix(int equiv_bitpix);

    /// Read only the FITS headers (no pixel data). Useful for scanning file lists.
    static FrameMetadata read_headers(const std::string& filepath);

    /// Every keyword of the primary header, name -> value as written, with a
    /// string's quotes removed and trailing blanks trimmed. COMMENT, HISTORY
    /// and blank cards are left out. Empty map if the file cannot be opened.
    static std::map<std::string, std::string> read_keywords(const std::string& filepath);

    /// The HISTORY cards of the primary header, in order.
    static std::vector<std::string> read_history(const std::string& filepath);

private:
    /// Extract metadata from FITS header keywords.
    static FrameMetadata extract_metadata(void* fptr);

    /// Read a float keyword, returning default_val if not found.
    static float read_float_key(void* fptr, const char* key, float default_val);

    /// Read an int keyword, returning default_val if not found.
    static int read_int_key(void* fptr, const char* key, int default_val);

    /// Read a string keyword, returning empty string if not found.
    static std::string read_string_key(void* fptr, const char* key);

    /// Read a float keyword; false (and `out` untouched) if it is absent.
    static bool try_read_float_key(void* fptr, const char* key, float& out);

    /// read(), read_fixed_scale() and read_raw(): the same open and pixel
    /// read, then that function's normalisation.
    enum class Scaling { PER_FRAME_MAX, FIXED, NONE };
    static FITSReadResult read_impl(const std::string& filepath, Scaling scaling);
};

} // namespace nukex
