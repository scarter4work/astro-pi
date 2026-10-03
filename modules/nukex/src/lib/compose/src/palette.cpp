#include "nukex/compose/palette.hpp"

#include <cstdlib>

namespace nukex {

LabColor Palette::for_line(EmissionLineId line) {
    switch (line) {
        // Hue angles: Ha 27 deg (red-orange), OIII -166 deg (teal), SII 11 deg
        // (deep red). Ha and OIII sit nearly OPPOSITE each other on purpose:
        // the composer blends by the Lab vector mean, so two lines that mix
        // desaturate toward a warm grey and white -- the additive HOO look --
        // instead of passing through a third hue. The previous OIII entry
        // (-15, -35) sat only 124 deg from Ha, and every Ha/OIII mix with 40%
        // or more OIII came out MAGENTA; the previous Ha (+50, +10) rendered
        // pink at mid lightness. Swatches at L* 35/55/75 for the candidates
        // are in the v5.0.5.3 notes.
        case EmissionLineId::Ha:   return {0.0, +50.0, +25.0};
        case EmissionLineId::OIII: return {0.0, -32.0,  -8.0};
        case EmissionLineId::SII:  return {0.0, +62.0, +12.0};
    }
    std::abort();
}

} // namespace nukex
