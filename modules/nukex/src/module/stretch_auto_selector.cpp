#include "stretch_auto_selector.hpp"
#include "nukex/io/filter_classifier.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include <sstream>

namespace nukex {

namespace {

std::unique_ptr<StretchOp> make_champion(FilterClass /*cls*/) {
    return std::make_unique<VeraLuxStretch>();
}

const char* champion_name(FilterClass /*cls*/) {
    return "VeraLux";
}

} // namespace

AutoSelection select_auto(const FrameMetadata& meta) {
    FilterClassifier classifier;
    const Filter f = classifier.classify(meta);

    AutoSelection sel;
    sel.op = make_champion(f.cls);
    std::ostringstream oss;
    oss << "Auto: classified as " << filter_class_name(f.cls)
        << " (FITS FILTER='" << meta.filter
        << "', BAYERPAT='" << meta.bayer_pattern
        << "', INSTRUME='" << meta.instrument
        << "') -> " << champion_name(f.cls);
    if (!classifier.last_warning().empty()) {
        oss << " | " << classifier.last_warning();
    }
    sel.log_line = oss.str();
    return sel;
}

AutoSelection select_auto(FilterClass cls) {
    AutoSelection sel;
    sel.op = make_champion(cls);
    std::ostringstream oss;
    oss << "Auto: classified as " << filter_class_name(cls)
        << " -> " << champion_name(cls);
    sel.log_line = oss.str();
    return sel;
}

} // namespace nukex
