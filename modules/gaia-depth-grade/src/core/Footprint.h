// Field footprint: the cone a catalogue query must cover (port of wcs.field_footprint).
#pragma once

#include "Sky.h"

namespace gdg {

struct Footprint {
    double center_ra = 0.0, center_dec = 0.0, radius_deg = 0.0;
};

/// astropy's angular_separation (the Vincenty formula SkyCoord.separation uses), degrees in and
/// out, with astropy's degree <-> radian scale factors and operation order.
double angular_separation_deg(double ra1, double dec1, double ra2, double dec2);

/// Centre = the sky at core pixel ((nx-1)/2, (ny-1)/2); radius = the largest separation from
/// it to the four corner pixel centres (0,0), (nx-1,0), (0,ny-1), (nx-1,ny-1).
/// Throws std::runtime_error when the transform cannot project one of those points.
Footprint field_footprint(const SkyTransform& sky, int ny, int nx);

}  // namespace gdg
