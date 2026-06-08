#include "filter_classifier.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace nukex {

namespace {

// Defensive — the FITS reader upper-cases the FILTER keyword before it
// reaches here, but keep the classifier robust against any future
// caller that forgets to normalise.  Cheap to do once per classify.
std::string to_upper(const std::string& s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

bool is_narrowband_name(const std::string& filter) {
    static const std::set<std::string> names{
        "HA", "H-ALPHA", "HALPHA", "H_ALPHA",
        "OIII", "O3", "O-III", "O_III",
        "SII", "S2", "S-II", "S_II",
        "NARROWBAND", "NB",
    };
    return names.find(to_upper(filter)) != names.end();
}

} // namespace

FilterClass classify_filter(const FITSMetadata& meta) {
    if (!meta.bayer_pat.empty()) return FilterClass::BAYER_RGB;
    if (is_narrowband_name(meta.filter)) return FilterClass::NARROWBAND;
    if (meta.naxis3 == 3) return FilterClass::LRGB_COLOR;
    return FilterClass::LRGB_MONO;
}

const char* filter_class_name(FilterClass c) {
    switch (c) {
        case FilterClass::LRGB_MONO:  return "LRGB-mono";
        case FilterClass::LRGB_COLOR: return "LRGB-color";
        case FilterClass::BAYER_RGB:  return "Bayer-RGB";
        case FilterClass::NARROWBAND: return "Narrowband";
    }
    return "unknown";
}

} // namespace nukex
