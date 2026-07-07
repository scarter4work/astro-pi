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

// NOTE (Task 14): the old filter_class_name(FilterClass) free function that
// used to live here was removed. It shared its exact mangled name with
// nukex::filter_class_name(nukex::FilterClass) in nukex/core/filter.hpp
// (the NEW 6-value FilterClass) -- both this module's old FilterClass and
// the lib's new FilterClass are named "nukex::FilterClass", so the two
// same-named-and-signatured functions collided at LINK time (an ODR
// violation invisible to the compiler, since each TU only ever sees one
// definition). NukeX-pxm.so was silently binding every filter_class_name
// call -- including the ones added by Task 14 that pass the NEW enum --
// to this OLD function body, printing wrong class names in the auto-select
// rationale log. This function was unused by anything except itself (grep
// confirmed no external caller), so removing it is the minimal correct
// fix. Do not re-add it without renaming, since filter_classifier.{hpp,cpp}
// itself is not going away until Task 19.

} // namespace nukex
