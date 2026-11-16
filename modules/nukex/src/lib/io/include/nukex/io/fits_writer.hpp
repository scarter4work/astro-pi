#pragma once

#include "nukex/io/image.hpp"
#include <string>
#include <vector>

namespace nukex {

/// One header keyword to write.
struct FITSCard {
    enum class Type { STRING, REAL, INTEGER, LOGICAL };
    std::string key;
    Type        type = Type::STRING;
    std::string text;            // STRING
    double      real = 0.0;      // REAL
    long long   integer = 0;     // INTEGER
    bool        logical = false; // LOGICAL
    std::string comment;

    static FITSCard str(std::string k, std::string v, std::string c = {}) {
        FITSCard f; f.key = std::move(k); f.type = Type::STRING; f.text = std::move(v); f.comment = std::move(c); return f;
    }
    static FITSCard num(std::string k, double v, std::string c = {}) {
        FITSCard f; f.key = std::move(k); f.type = Type::REAL; f.real = v; f.comment = std::move(c); return f;
    }
    static FITSCard integ(std::string k, long long v, std::string c = {}) {
        FITSCard f; f.key = std::move(k); f.type = Type::INTEGER; f.integer = v; f.comment = std::move(c); return f;
    }
    static FITSCard flag(std::string k, bool v, std::string c = {}) {
        FITSCard f; f.key = std::move(k); f.type = Type::LOGICAL; f.logical = v; f.comment = std::move(c); return f;
    }
};

/// Writes a float32 FITS image (NAXIS3 = channels when there is more than one).
///
/// The file is written beside its destination under a temporary name and
/// renamed into place, so a reader -- or a cache that trusts a file it finds
/// -- never sees a half-written one. Opens with fits_create_diskfile(), which
/// does no filename parsing (no URL drivers, no "!" overwrite syntax).
class FITSWriter {
public:
    /// false, with the reason in `error`, if anything fails; the destination
    /// is then left as it was.
    static bool write_float(const std::string& path, const Image& image,
                            const std::vector<FITSCard>& cards,
                            const std::vector<std::string>& history,
                            std::string& error);
};

} // namespace nukex
