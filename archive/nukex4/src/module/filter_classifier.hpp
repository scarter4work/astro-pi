// NukeX v4 — Classifies FITS metadata into one of four filter classes
// used for stretch Auto-selection.
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __NukeX_filter_classifier_h
#define __NukeX_filter_classifier_h

#include "fits_metadata.hpp"

namespace nukex {

enum class FilterClass {
    LRGB_MONO,
    LRGB_COLOR,
    BAYER_RGB,
    NARROWBAND,
};

FilterClass classify_filter(const FITSMetadata& meta);
const char* filter_class_name(FilterClass c);

} // namespace nukex

#endif
