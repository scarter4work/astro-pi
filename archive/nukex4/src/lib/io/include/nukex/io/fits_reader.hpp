#pragma once

#include "nukex/io/image.hpp"
#include "nukex/core/frame_metadata.hpp"
#include <string>

namespace nukex {

/// Result of reading a FITS file.
struct FITSReadResult {
    Image         image;     // pixel data as float32
    FrameMetadata metadata;  // extracted header information
    bool          success = false;
    std::string   error;     // error message if !success
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

    /// Read only the FITS headers (no pixel data). Useful for scanning file lists.
    static FrameMetadata read_headers(const std::string& filepath);

private:
    /// Extract metadata from FITS header keywords.
    static FrameMetadata extract_metadata(void* fptr);

    /// Read a float keyword, returning default_val if not found.
    static float read_float_key(void* fptr, const char* key, float default_val);

    /// Read an int keyword, returning default_val if not found.
    static int read_int_key(void* fptr, const char* key, int default_val);

    /// Read a string keyword, returning empty string if not found.
    static std::string read_string_key(void* fptr, const char* key);
};

} // namespace nukex
