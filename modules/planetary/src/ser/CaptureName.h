#pragma once
#include <optional>
#include <string>

#include "FrameSource.h"

namespace planetary::ser {

enum class Target { Other, Moon, Mercury, Venus, Mars, Jupiter, Saturn, Uranus, Neptune };
enum class FilterClass { Unknown, Broadband, Narrowband };

struct CaptureName {
    bool parsed = false;          // the ASIAIR pattern matched
    Target target = Target::Other;
    std::string targetToken;      // as written, e.g. "Moon", "Betelgeuse"
    std::string filterToken;      // e.g. "Lqef", "HaO3"; empty when absent
    FilterClass filter = FilterClass::Unknown;
};

// Parses `YYYY-MM-DD-HHMMSS-<Target>-Bin<n> <Filter> <temp>F` + extension (ASIAIR's capture name;
// PIPP keeps it, so a trailing `_pipp` before the extension is fine). `fileName` is a base name
// or a full path. Target and filter tokens match case-insensitively; an unknown target is Other
// (parsed stays true), an unknown filter token is Unknown.
CaptureName ParseCaptureName(const std::string& fileName);

// "Moon", "Jupiter", ..., "Other".
const char* TargetName(Target);

// nullopt when Mineral moon may run; else the ASCII reason shown in the tooltip and the failure.
// `filterToken` (CaptureName::filterToken) names the filter in the narrowband reason; empty says "the filter".
std::optional<std::string> MineralMoonRefusal(Target target, FilterClass filter, const FileInfo& info,
                                              const std::string& filterToken);

}  // namespace planetary::ser
