#include "nukex/compose/palette.hpp"

#include <cstdlib>

namespace nukex {

LabColor Palette::for_line(EmissionLineId line) {
    switch (line) {
        case EmissionLineId::Ha:   return {0.0, +50.0, +10.0};
        case EmissionLineId::OIII: return {0.0, -15.0, -35.0};
        case EmissionLineId::SII:  return {0.0, +60.0, +25.0};
    }
    std::abort();
}

} // namespace nukex
