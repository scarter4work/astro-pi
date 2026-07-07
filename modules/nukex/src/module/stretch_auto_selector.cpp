#include "stretch_auto_selector.hpp"
#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/core/frame_metadata.hpp"
#include <sstream>

namespace nukex {

namespace {

std::unique_ptr<StretchOp> make_champion(FilterClass /*cls*/) {
    return std::make_unique<VeraLuxStretch>();
}

const char* champion_name(FilterClass /*cls*/) {
    return "VeraLux";
}

// Minimal FITSMetadata -> FrameMetadata bridge. FilterClassifier::classify()
// only reads .filter and .bayer_pattern (see filter_classifier.cpp), so
// that's all this populates -- no other FrameMetadata field is fabricated.
FrameMetadata to_frame_metadata(const FITSMetadata& meta) {
    FrameMetadata fm;
    fm.filter        = meta.filter;
    fm.bayer_pattern = meta.bayer_pat;
    return fm;
}

} // namespace

AutoSelection select_auto(const FITSMetadata& meta) {
    FilterClassifier classifier;
    FilterClass cls = classifier.classify(to_frame_metadata(meta)).cls;
    AutoSelection sel;
    sel.op = make_champion(cls);
    std::ostringstream oss;
    oss << "Auto: classified as " << filter_class_name(cls)
        << " (FITS FILTER='" << meta.filter
        << "', BAYERPAT='" << meta.bayer_pat
        << "', NAXIS3=" << meta.naxis3
        << ") -> " << champion_name(cls);
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
