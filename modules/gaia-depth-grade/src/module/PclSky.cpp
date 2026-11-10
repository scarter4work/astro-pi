#include "PclSky.h"

#include <pcl/ErrorHandler.h>

#include <cmath>

namespace pcl
{

PclSky::PclSky( const AstrometricMetadata& metadata )
   : m_metadata( metadata )
{
   if ( !m_metadata.IsValid() )
      throw Error( "Gaia Depth Grade: the image has no valid astrometric solution." );
}

bool PclSky::pixel_to_sky( double x, double y, double& ra, double& dec ) const
{
   DPoint rd;
   if ( !m_metadata.ImageToCelestial( rd, CoreToPI( x, y ) ) )
      return false;
   ra = rd.x;
   dec = rd.y;
   return std::isfinite( ra ) && std::isfinite( dec );
}

bool PclSky::sky_to_pixel( double ra, double dec, double& x, double& y ) const
{
   DPoint pI;
   if ( !m_metadata.CelestialToImage( pI, DPoint( ra, dec ) ) )
      return false;
   PIToCore( pI, x, y );
   return std::isfinite( x ) && std::isfinite( y );
}

} // namespace pcl
