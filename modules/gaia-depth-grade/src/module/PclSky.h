// PclSky: a PixInsight astrometric solution as a gdg::SkyTransform (plan Task 11).
//
// The core works in the Python core's numpy frame, which is PixInsight's row order: PixInsight
// saves FITS with ROWORDER = 'TOP-DOWN' by default, so astropy's data[0] is PixInsight's top row
// (measured 2026-11-10), and the product's writeWcsKeywords mirrored the solution's bottom-up
// WCS to match. Pixel centres sit on integers (astropy origin 0); PixInsight image coordinates
// put the centre of the top-left pixel at (0.5, 0.5). So:
//
//    x_pi = x + 0.5          y_pi = y + 0.5
//
// Proven against the exported linear WCS by the in-PI probe `sky_adapter_convention`
// (test/run-probe.sh), which also rejects the bottom-up mapping y_pi = H - y - 0.5 by ~1080 px.
#ifndef __GDG_PclSky_h
#define __GDG_PclSky_h

#include "core/Sky.h"

#include <pcl/AstrometricMetadata.h>

namespace pcl
{

class PclSky : public gdg::SkyTransform
{
public:

   /// Throws pcl::Error when `metadata` holds no valid astrometric solution.
   explicit PclSky( const AstrometricMetadata& metadata );

   bool pixel_to_sky( double x, double y, double& ra, double& dec ) const override;
   bool sky_to_pixel( double ra, double dec, double& x, double& y ) const override;

   /// The core <-> PixInsight pixel mapping, exposed for the probe.
   static DPoint CoreToPI( double x, double y ) { return DPoint( x + 0.5, y + 0.5 ); }
   static void PIToCore( const DPoint& p, double& x, double& y ) { x = p.x - 0.5; y = p.y - 0.5; }

private:

   AstrometricMetadata m_metadata;
};

} // namespace pcl

#endif // __GDG_PclSky_h
