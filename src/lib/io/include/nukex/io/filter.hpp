#ifndef NUKEX_IO_FILTER_HPP
#define NUKEX_IO_FILTER_HPP

#include <string>

namespace nukex {

enum class FilterClass : int {
    UNKNOWN            = 0,
    BROADBAND_L        = 1,
    BROADBAND_RGB      = 2,
    BROADBAND_OSC      = 3,
    NARROWBAND_SINGLE  = 4,
    DUAL_NB_OSC        = 5,
};

const char* filter_class_name(FilterClass c);

struct BandwidthSpec {
    double center_nm = 0.0;
    double fwhm_nm   = 0.0;
};

struct Filter {
    FilterClass    cls = FilterClass::UNKNOWN;
    std::string    name;
    std::string    camera;
    BandwidthSpec  bandwidth;
};

} // namespace nukex

#endif
