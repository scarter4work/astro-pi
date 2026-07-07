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
// NOTE (Task 14): filter_class_name(FilterClass) was removed from here --
// see the comment above classify_filter's definition in filter_classifier.cpp
// for why (it collided at link time with nukex::filter_class_name in
// nukex/core/filter.hpp, both being "nukex::filter_class_name(nukex::FilterClass)").

} // namespace nukex

#endif
