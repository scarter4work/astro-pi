#include "nukex/io/filter.hpp"

#include <cstdlib>

namespace nukex {

const char* filter_class_name(FilterClass c) {
    switch (c) {
        case FilterClass::UNKNOWN:           return "UNKNOWN";
        case FilterClass::BROADBAND_L:       return "BROADBAND_L";
        case FilterClass::BROADBAND_RGB:     return "BROADBAND_RGB";
        case FilterClass::BROADBAND_OSC:     return "BROADBAND_OSC";
        case FilterClass::NARROWBAND_SINGLE: return "NARROWBAND_SINGLE";
        case FilterClass::DUAL_NB_OSC:       return "DUAL_NB_OSC";
    }
    std::abort();
}

} // namespace nukex
