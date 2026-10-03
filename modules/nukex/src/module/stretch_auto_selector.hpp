#ifndef __NukeX_stretch_auto_selector_h
#define __NukeX_stretch_auto_selector_h

#include "nukex/core/filter.hpp"
#include "nukex/core/frame_metadata.hpp"
#include "nukex/stretch/stretch_op.hpp"
#include <memory>
#include <string>

namespace nukex {

struct AutoSelection {
    std::unique_ptr<StretchOp> op;
    std::string log_line;
};

/// Primary entry point: classify (lib FilterClassifier) + select + build a
/// rationale log line. `meta` populates the log with the FITS header values
/// that drove the classification (FILTER / BAYERPAT / INSTRUME) so a user
/// can trace "why BROADBAND_L?" in the Process Console.
AutoSelection select_auto(const FrameMetadata& meta);

/// FilterClass-only overload (empty header detail in the log line).
AutoSelection select_auto(FilterClass cls);

} // namespace nukex

#endif
