#ifndef __NukeX_stretch_auto_selector_h
#define __NukeX_stretch_auto_selector_h

#include "filter_classifier.hpp"
#include "fits_metadata.hpp"
#include "nukex/stretch/stretch_op.hpp"
#include <memory>
#include <string>

namespace nukex {

struct AutoSelection {
    std::unique_ptr<StretchOp> op;
    std::string log_line;
};

/// Primary entry point: classify + select + build a rationale log line.
/// The `meta` is used only to populate the log line with the FITS header
/// values that actually drove the classification, so a user reading the
/// Process Console can trace "why LRGB-mono?" back to FILTER/BAYERPAT/NAXIS3.
AutoSelection select_auto(const FITSMetadata& meta);

/// Backward-compat overload (FilterClass-only; empty log detail).
AutoSelection select_auto(FilterClass cls);

} // namespace nukex

#endif
