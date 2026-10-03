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

// Broadband names whose class depends on the sensor: OSC on a Bayer frame,
// luminance on a mono frame. Normalised (lowercase, alphanumerics only).
// Sources: research/qe_database_research.json types `luminance` and
// `broadband-LPR`, plus the bare L aliases moved out of known_table().
const std::unordered_set<std::string>& broadband_any_names() {
    static const std::unordered_set<std::string> names = {
        "l", "lum", "luminance",
        "lpro", "lpr", "lps", "lpsd1", "lpsd2", "lpsd3", "lpsv4",

        // Quad-band light-pollution glass. It passes Ha, OIII and SII, which
        // is why v5.0.0.2 filed it as narrowband -- but it passes ~175 nm to
        // do it, against 3 nm for L-Ultimate, 5 for ALP-T and 7 for
        // L-eXtreme. A 175 nm passband is not narrowband, and the filter says
        // what light gets through, not what the target emits: asked to
        // decompose the continuum of a galaxy into three emission lines, the
        // Q-solve drove one line to zero across a whole 114-frame stack of
        // M63 and the picture came out teal. This is what people image RGB
        // with, so it is classified as what it is.
        //
        // Note what this costs: the three-line Q-solve now has no shipped
        // spelling at all, and a taught alias cannot restore one, because
        // aliases are consulted only when the table has no answer (see
        // classify()). ChannelDecomposer still supports three lines; reaching
        // it again needs a deliberate opt-in rather than a filter name.
        "hao3s2", "haoiiisii", "lqef", "lquad", "lquadenhance",
        "optolonglquadenhance", "lsynergy",
        "uvir", "uvircut", "uvirblock", "irblock", "uvcut",
        "cls", "clsccd",
        "l1", "l2", "l3",           // Astronomik L1/L2/L3 UV-IR block
        "ircut", "luvir", "optolonglpro",

        // Filter-wheel slot labels that mean "no narrowband isolation": a
        // clear slot, or the user's own label for an L-Pro / L-QEF slot
        // ("Full"). Without these a Bayer frame labelled "Full" classified
        // as UNKNOWN and hard-aborted the stack (astro-pi NukeX 5.1.0.1).
        "full", "clear", "none", "open", "nofilter", "no",
    };
    return names;
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

    if (normalized.empty()) {
        if (is_bayer) {
            out.cls       = FilterClass::BROADBAND_OSC;
            out.name      = "OSC";
            out.bandwidth = BandwidthSpec{550.0, 300.0};
        } else {
            out.cls       = FilterClass::BROADBAND_L;
            out.name      = "L_unnamed";
            out.bandwidth = BandwidthSpec{550.0, 300.0};
        }
        return out;
    }

    if (broadband_any_names().count(normalized)) {
        if (is_bayer) {
            out.cls  = FilterClass::BROADBAND_OSC;
            out.name = "OSC";
        } else {
            out.cls  = FilterClass::BROADBAND_L;
            out.name = "L";
        }
        out.bandwidth = BandwidthSpec{550.0, 300.0};
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

    // Not in the shipped table. Before giving up, consult what the user has
    // taught us: the interface offers to learn an unrecognised name by asking
    // which emission lines it passes, and records the answer against the
    // normalised header value. Checked here, last, so a taught name can only
    // add a spelling -- it can never redefine Ha or HaO3.
    const std::string taught = aliases_.lookup(normalized);
    if (!taught.empty()) {
        cls = lookup_known(FilterAliasStore::normalize(taught), canonical, bw);
        if (cls != FilterClass::UNKNOWN) {
            out.cls       = cls;
            out.name      = canonical;
            out.bandwidth = bw;
            return out;
        }
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
                      + "If this is a narrowband filter, rename FILTER to Ha/OIII/SII or add it to a qe_overrides.json selected in the NukeX interface.";
    }
    return out;
}

} // namespace nukex
