// The online Bailer-Jones query text (port of distances.build_adql), byte for byte.
#pragma once

#include "Footprint.h"

#include <optional>
#include <string>

namespace gdg {

/// Python's repr(float) (float_repr_style 'short'): the shortest digits that round-trip, in
/// fixed notation when the decimal exponent is in (-4, 16] (with ".0" for integral values),
/// else d[.ddd]e[+-]XX. Throws std::invalid_argument for NaN or infinity.
std::string py_float_repr(double v);

/// distances.build_adql: Gaia DR3 joined with external.gaiaedr3_distance over the footprint
/// cone, r_med_geo and G not null, optional G > mag_gt and G < mag_lt, ordered by G.
std::string build_adql(const Footprint& fp, long top, std::optional<double> mag_gt, std::optional<double> mag_lt);

}  // namespace gdg
