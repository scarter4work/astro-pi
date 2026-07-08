#include "nukex/io/filter_classifier.hpp"
#include "nukex/core/frame_metadata.hpp"

#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace nukex {

namespace {

struct KnownFilter {
    FilterClass    cls;
    const char*    canonical;
    double         center_nm;
    double         fwhm_nm;
};

const std::unordered_map<std::string, KnownFilter>& known_table() {
    static const std::unordered_map<std::string, KnownFilter> table = {
        {"l",         {FilterClass::BROADBAND_L,       "L",   550.0, 300.0}},
        {"luminance", {FilterClass::BROADBAND_L,       "L",   550.0, 300.0}},
        {"r",         {FilterClass::BROADBAND_RGB,     "R",   620.0, 100.0}},
        {"red",       {FilterClass::BROADBAND_RGB,     "R",   620.0, 100.0}},
        {"g",         {FilterClass::BROADBAND_RGB,     "G",   540.0, 100.0}},
        {"green",     {FilterClass::BROADBAND_RGB,     "G",   540.0, 100.0}},
        {"b",         {FilterClass::BROADBAND_RGB,     "B",   460.0, 100.0}},
        {"blue",      {FilterClass::BROADBAND_RGB,     "B",   460.0, 100.0}},

        {"ha",        {FilterClass::NARROWBAND_SINGLE, "Ha",   656.3,   7.0}},
        {"halpha",    {FilterClass::NARROWBAND_SINGLE, "Ha",   656.3,   7.0}},
        {"oiii",      {FilterClass::NARROWBAND_SINGLE, "OIII", 500.7,   7.0}},
        {"o3",        {FilterClass::NARROWBAND_SINGLE, "OIII", 500.7,   7.0}},
        {"sii",       {FilterClass::NARROWBAND_SINGLE, "SII",  672.4,   7.0}},
        {"s2",        {FilterClass::NARROWBAND_SINGLE, "SII",  672.4,   7.0}},

        {"hao3",      {FilterClass::DUAL_NB_OSC,       "HaO3", 578.5, 155.0}},
        {"haoiii",    {FilterClass::DUAL_NB_OSC,       "HaO3", 578.5, 155.0}},
        {"s2o3",      {FilterClass::DUAL_NB_OSC,       "S2O3", 586.0, 170.0}},
        {"siioiii",   {FilterClass::DUAL_NB_OSC,       "S2O3", 586.0, 170.0}},
        {"lextreme",  {FilterClass::DUAL_NB_OSC,       "L-eXtreme",  578.5,  7.0}},
        {"lenhance",  {FilterClass::DUAL_NB_OSC,       "L-eNhance",  578.5, 25.0}},
        {"lultimate", {FilterClass::DUAL_NB_OSC,       "L-Ultimate", 578.5,  3.0}},
        {"alpt",      {FilterClass::DUAL_NB_OSC,       "ALP-T",      578.5,  5.0}},
    };
    return table;
}

// Filter-wheel slot labels that denote NO narrowband isolation — i.e. a clear
// slot or a broadband light-pollution / UV-IR-cut filter. These pass the full
// visible band, so for channel routing they are equivalent to "no filter":
// Bayer -> BROADBAND_OSC (plain RGB debayer), mono -> BROADBAND_L. Recognising
// them here prevents a spurious UNKNOWN-on-Bayer hard-abort (they are not
// narrowband filters, so there is nothing to mis-colour by treating them as OSC).
// Normalized form = lowercased alphanumerics only (see normalize_name).
bool is_clear_broadband(const std::string& normalized) {
    static const std::unordered_set<std::string> clear = {
        "full",                                   // Optolong L-Pro / L-QEF slot label (user kit)
        "clear", "none", "open", "nofilter", "no", // generic no-filter slots
        "lpro", "optolonglpro",                   // Optolong L-Pro (broadband LP)
        "lqef", "lquadenhance", "lquad",          // Optolong L-Quad Enhance
        "uvir", "uvircut", "ircut", "luvir",      // UV/IR-cut (broadband)
    };
    return clear.count(normalized) != 0;
}

} // namespace

std::string FilterClassifier::normalize_name(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    return out;
}

FilterClass FilterClassifier::lookup_known(const std::string& normalized,
                                           std::string& canonical_name_out,
                                           BandwidthSpec& bandwidth_out) {
    const auto& table = known_table();
    auto it = table.find(normalized);
    if (it == table.end()) return FilterClass::UNKNOWN;
    canonical_name_out      = it->second.canonical;
    bandwidth_out.center_nm = it->second.center_nm;
    bandwidth_out.fwhm_nm   = it->second.fwhm_nm;
    return it->second.cls;
}

Filter FilterClassifier::classify(const FrameMetadata& meta) {
    last_warning_.clear();

    const bool        is_bayer  = !meta.bayer_pattern.empty();
    const std::string normalized = normalize_name(meta.filter);

    Filter out;
    out.camera = meta.instrument;

    // Empty filter, or a clear/broadband-LP slot label (e.g. "Full", "Clear",
    // L-Pro, L-QEF): no narrowband isolation -> plain broadband routing. Keep
    // the original label for logging when the frame named a filter.
    if (normalized.empty() || is_clear_broadband(normalized)) {
        if (is_bayer) {
            out.cls       = FilterClass::BROADBAND_OSC;
            out.name      = normalized.empty() ? "OSC" : meta.filter;
            out.bandwidth = BandwidthSpec{550.0, 300.0};
        } else {
            out.cls       = FilterClass::BROADBAND_L;
            out.name      = normalized.empty() ? "L_unnamed" : meta.filter;
            out.bandwidth = BandwidthSpec{550.0, 300.0};
        }
        return out;
    }

    std::string   canonical;
    BandwidthSpec bw;
    FilterClass   cls = lookup_known(normalized, canonical, bw);
    if (cls != FilterClass::UNKNOWN) {
        out.cls       = cls;
        out.name      = canonical;
        out.bandwidth = bw;
        return out;
    }

    if (is_bayer) {
        out.cls  = FilterClass::UNKNOWN;
        out.name = meta.filter;
    } else {
        out.cls       = FilterClass::BROADBAND_L;
        out.name      = meta.filter;
        out.bandwidth = BandwidthSpec{550.0, 300.0};
        last_warning_ = "Unknown filter '" + meta.filter
                      + "' for mono frame -- treating as generic luminance. "
                      + "If this is a narrowband filter, add it to qe_overrides.json.";
    }
    return out;
}

} // namespace nukex
