// NukeX v4 — FITS metadata extraction for module-layer stretch selection.
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeX_fits_metadata_h
#define __NukeX_fits_metadata_h

#include <string>

namespace nukex {

struct FITSMetadata {
    std::string filter;     // FITS "FILTER" keyword, uppercased, trimmed. Empty if absent.
    std::string bayer_pat;  // FITS "BAYERPAT" keyword, uppercased, trimmed. Empty if absent.
    int naxis3 = 1;         // FITS "NAXIS3" keyword. Defaults to 1 (mono) if absent.
    bool read_ok = false;   // True if the file opened and header read without error.
};

/// Read the primary HDU header of a FITS file at `path` and return its
/// metadata. On I/O or parse error, returns a default-constructed
/// FITSMetadata with read_ok=false.
FITSMetadata read_fits_metadata(const std::string& path);

} // namespace nukex

#endif // __NukeX_fits_metadata_h
