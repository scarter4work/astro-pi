#include "CaptureName.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

#include "AsiairSidecar.h"
#include "SerFile.h"

namespace planetary::ser {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsOneOf(const std::string& token, std::initializer_list<const char*> list) {
    const std::string t = Lower(token);
    for (const char* s : list)
        if (t == Lower(s)) return true;
    return false;
}

// An infrared-pass filter: "IR" and a near-infrared cut-on in nm (IR642, IR-685, IR_850, IR685nm, IR1000), "IRPass"
// with or without one, or Astronomik's ProPlanet (PP642, ProPlanet-742). Case, '-' and '_' are ignored.
bool IsIrPass(const std::string& token) {
    std::string t;
    for (char c : Lower(token))
        if (c != '-' && c != '_') t += c;
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "nm") == 0) t.resize(t.size() - 2);
    auto cutOn = [&](size_t at) {  // the rest is a wavelength of 600 - 1100 nm
        if (at >= t.size() || t.size() - at > 4 ||
            !std::all_of(t.begin() + static_cast<long>(at), t.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            return false;
        const int nm = std::stoi(t.substr(at));
        return nm >= 600 && nm <= 1100;
    };
    if (t.rfind("irpass", 0) == 0) return t.size() == 6 || cutOn(6);
    if (t.rfind("proplanet", 0) == 0) return cutOn(9);
    if (t.rfind("pp", 0) == 0) return cutOn(2);
    return t.rfind("ir", 0) == 0 && cutOn(2);
}

FilterClass Classify(const std::string& token) {
    if (IsOneOf(token, {"HaO3", "S2O3", "Ha", "OIII", "O3", "SII", "S2", "Hb", "D1", "D2", "Duo", "NBZ",
                        "ALP-T", "L-eNhance", "L-eXtreme", "L-Ultimate", "CH4", "Methane"}))
        return FilterClass::Narrowband;
    if (IsOneOf(token, {"Lqef", "LPro", "L-Pro", "L", "UVIR", "UV/IR", "IRCut", "None", "Clear"}))
        return FilterClass::Broadband;
    if (IsIrPass(token)) return FilterClass::IrPass;
    return FilterClass::Unknown;
}

Target TargetFromToken(const std::string& token) {
    static const struct { const char* name; Target t; } kTargets[] = {
        {"moon", Target::Moon},       {"mercury", Target::Mercury}, {"venus", Target::Venus},
        {"mars", Target::Mars},       {"jupiter", Target::Jupiter}, {"saturn", Target::Saturn},
        {"uranus", Target::Uranus},   {"neptune", Target::Neptune}};
    const std::string t = Lower(token);
    for (const auto& e : kTargets)
        if (t == e.name) return e.t;
    return Target::Other;
}

// Start of the base name inside a path: the latest separator-following position where the
// timestamp prefix begins (a filter such as `UV/IR` holds a separator), else after the last one.
size_t BaseStart(const std::string& path) {
    size_t fallback = 0;
    for (size_t i = path.size(); i-- > 0;) {
        if (path[i] != '/' && path[i] != '\\') continue;
        if (fallback == 0) fallback = i + 1;
        if (FilenameStart(path.substr(i + 1))) return i + 1;
    }
    return FilenameStart(path) ? 0 : fallback;
}

// `-4.0F` / `64.6F`
size_t TempEnd(const std::string& s, size_t i) {
    const size_t b = i;
    if (i < s.size() && s[i] == '-') ++i;
    size_t digits = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i, ++digits;
    if (digits == 0) return b;
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    }
    return (i < s.size() && s[i] == 'F') ? i + 1 : b;
}

}  // namespace

CaptureName ParseCaptureName(const std::string& path) {
    CaptureName out;
    const std::string name = path.substr(BaseStart(path));
    if (!FilenameStart(name)) return out;

    // <Target>-Bin<n> <Filter> <temp>F
    const size_t binAt = name.find("-Bin", 18);
    if (binAt == std::string::npos || binAt == 18) return out;
    size_t i = binAt + 4;
    const size_t digitsAt = i;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i == digitsAt || i >= name.size() || name[i] != ' ') return out;
    ++i;
    const size_t filterAt = i;
    const size_t sp = name.find(' ', filterAt);
    if (sp == std::string::npos || sp == filterAt) return out;
    const size_t end = TempEnd(name, sp + 1);
    if (end == sp + 1) return out;
    if (end < name.size() && name[end] != '.' && name[end] != '_') return out;

    out.parsed = true;
    out.targetToken = name.substr(18, binAt - 18);
    out.target = TargetFromToken(out.targetToken);
    out.filterToken = name.substr(filterAt, sp - filterAt);
    out.filter = Classify(out.filterToken);
    return out;
}

const char* TargetName(Target t) {
    switch (t) {
        case Target::Moon: return "Moon";
        case Target::Mercury: return "Mercury";
        case Target::Venus: return "Venus";
        case Target::Mars: return "Mars";
        case Target::Jupiter: return "Jupiter";
        case Target::Saturn: return "Saturn";
        case Target::Uranus: return "Uranus";
        case Target::Neptune: return "Neptune";
        case Target::Other: break;
    }
    return "Other";
}

std::optional<std::string> MineralMoonRefusal(Target target, FilterClass filter, const FileInfo& info,
                                              const std::string& filterToken) {
    if (target != Target::Moon)
        return std::string("Mineral moon is for the Moon; the target is ") + TargetName(target) + ".";
    if (info.planes == 1 && !IsBayer(info.colorId))
        return std::string(
            "Mineral moon needs a colour capture; this one is mono (set the Bayer pattern if it is a "
            "colour camera).");
    if (filter == FilterClass::Narrowband || filter == FilterClass::IrPass)
        return "Mineral moon needs a broadband filter; " +
               (filterToken.empty() ? std::string("the filter") : filterToken) +
               (filter == FilterClass::Narrowband ? " is narrowband" : " is IR-pass") +
               ", so its colours come from the filter.";
    return std::nullopt;
}

}  // namespace planetary::ser
