// Port of gaia_depth_grade/wcs.py field_footprint and astropy's angular_separation.
#include "Footprint.h"

#include "Format.h"

#include <cmath>
#include <stdexcept>

namespace gdg {

namespace {

// astropy.units: degree = (np.pi / 180.0) rad. np.sin on a degree Quantity multiplies by that
// scale; Angle(<radians>, unit=deg) multiplies by radian._to(degree) = 1.0 / that scale.
constexpr double kDegToRad = 3.141592653589793 / 180.0;
constexpr double kRadToDeg = 1.0 / kDegToRad;

void project(const SkyTransform& sky, double x, double y, double& ra, double& dec) {
    if (!sky.pixel_to_sky(x, y, ra, dec) || !std::isfinite(ra) || !std::isfinite(dec))
        throw std::runtime_error(
            strformat("field footprint: the astrometric solution cannot project image pixel (%.1f, %.1f)", x, y));
}

}  // namespace

double angular_separation_deg(double ra1, double dec1, double ra2, double dec2) {
    // astropy.coordinates.angles.utils.angular_separation, operation for operation; the
    // longitude difference is taken in degrees, then converted (np.sin(lon2 - lon1)).
    const double dlon = (ra2 - ra1) * kDegToRad;
    const double sdlon = std::sin(dlon);
    const double cdlon = std::cos(dlon);
    const double slat1 = std::sin(dec1 * kDegToRad);
    const double slat2 = std::sin(dec2 * kDegToRad);
    const double clat1 = std::cos(dec1 * kDegToRad);
    const double clat2 = std::cos(dec2 * kDegToRad);

    const double num1 = clat2 * sdlon;
    const double num2 = clat1 * slat2 - slat1 * clat2 * cdlon;
    const double denominator = slat1 * slat2 + clat1 * clat2 * cdlon;

    return std::atan2(std::hypot(num1, num2), denominator) * kRadToDeg;
}

Footprint field_footprint(const SkyTransform& sky, int ny, int nx) {
    if (ny < 1 || nx < 1) throw std::invalid_argument("field_footprint: empty image");
    const double cx = (nx - 1) / 2.0, cy = (ny - 1) / 2.0;
    Footprint fp;
    project(sky, cx, cy, fp.center_ra, fp.center_dec);
    const double corners_x[4] = {0.0, static_cast<double>(nx - 1), 0.0, static_cast<double>(nx - 1)};
    const double corners_y[4] = {0.0, 0.0, static_cast<double>(ny - 1), static_cast<double>(ny - 1)};
    double radius = 0.0;
    for (int i = 0; i < 4; ++i) {
        double ra, dec;
        project(sky, corners_x[i], corners_y[i], ra, dec);
        const double sep = angular_separation_deg(fp.center_ra, fp.center_dec, ra, dec);
        if (i == 0 || sep > radius) radius = sep;  // np.max
    }
    fp.radius_deg = radius;
    return fp;
}

}  // namespace gdg
