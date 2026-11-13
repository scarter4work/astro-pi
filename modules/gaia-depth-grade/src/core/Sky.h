// The celestial <-> pixel transform the core works through (plan Task 11).
//
// Coordinates are core-frame pixels: the Python core's numpy frame, x = column, y = row with
// y = 0 PixInsight's top row (frame contract C1), pixel centres on integers (astropy's origin-0
// convention). The module's PclSky adapts a PixInsight astrometric solution to this frame as
// x_pi = x + 0.5, y_pi = y + 0.5 (no vertical flip); tests use TanSky (astropy's linear TAN WCS)
// or a lookup table.
#pragma once

namespace gdg {

class SkyTransform {
public:
    virtual ~SkyTransform() = default;
    /// Core pixel (x, y) to ICRS (ra, dec) in degrees. False when the point cannot be projected.
    virtual bool pixel_to_sky(double x, double y, double& ra, double& dec) const = 0;
    /// ICRS (ra, dec) in degrees to core pixel (x, y). False when the point cannot be projected.
    virtual bool sky_to_pixel(double ra, double dec, double& x, double& y) const = 0;
};

}  // namespace gdg
