// TEST-ONLY probe case `sky_adapter_convention` (plan Task 11).
//
// What the Python core saw: the stars layer saved by PixInsight as FITS (default settings write
// ROWORDER = 'TOP-DOWN': numpy row 0 is PixInsight's top row) with the linear WCS keywords the
// product's writeWcsKeywords exported: ImageSolver's GetWCSvalues() (standard bottom-up FITS WCS
// of the solution's linear approximation), then CRPIX2 -> H+1-CRPIX2 and CD1_2, CD2_2 negated.
// Here PCL generates the same linear WCS (AstrometricMetadata::UpdateWCSKeywords, the formula of
// GetWCSvalues) and WCSKeywords::MirrorVertically applies exactly that flip. TanSky (astropy's
// TAN WCS) over those keywords is then compared with PclSky in core-frame pixels, and the wrong
// conventions are evaluated too, to show the probe tells them apart. ImageBridge is checked
// against the same row order.
#include "ProbeJson.h"
#include "ProbeProcess.h"

#include "../ImageBridge.h"
#include "../PclSky.h"
#include "TanSky.h"

#include <pcl/AstrometricMetadata.h>
#include <pcl/ErrorHandler.h>
#include <pcl/FITSHeaderKeyword.h>
#include <pcl/ImageWindow.h>
#include <pcl/View.h>
#include <pcl/WCSKeywords.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <string>

namespace pcl
{

using namespace gdgprobe;

namespace
{

struct Err
{
   double max = 0, sum = 0;
   int n = 0;
   void Add( double e )
   {
      if ( !std::isfinite( e ) ) e = 1e30;
      max = std::max( max, e );
      sum += e;
      ++n;
   }
   double Mean() const { return n ? sum/n : 0; }
};

} // namespace

IsoString ProbeSkyAdapterConvention( View& view, const String& )
{
   ImageWindow window = view.Window();
   AstrometricMetadata md( window );
   if ( !md.IsValid() )
      throw Error( "the view has no valid astrometric solution" );
   ImageVariant image = view.Image();
   const int W = image->Width(), H = image->Height();
   const PclSky sky( md );

   // The exported linear WCS, as writeWcsKeywords wrote it for the Python core.
   FITSKeywordArray keywords;
   md.UpdateWCSKeywords( keywords, true/*generate*/ );
   WCSKeywords::MirrorVertically( keywords, H );
   std::map<std::string, double> k;
   IsoString ctype1;
   for ( const FITSHeaderKeyword& kw : keywords )
   {
      const IsoString name = kw.name.Trimmed();
      if ( name == "CTYPE1" )
         ctype1 = kw.StripValueDelimiters();
      static const char* numeric[] = { "CRPIX1", "CRPIX2", "CRVAL1", "CRVAL2", "CD1_1", "CD1_2", "CD2_1", "CD2_2",
                                        "LONPOLE", "PV1_1", "PV1_2" };
      for ( const char* n : numeric )
         if ( name == n )
         {
            double v;
            if ( kw.GetNumericValue( v ) )
               k[ n ] = v;
         }
   }
   if ( !ctype1.Contains( "TAN" ) )
      throw Error( "the generated WCS keywords are not TAN (CTYPE1 = '" + String( ctype1 ) + "')" );
   const gdg::test::TanSky tan = gdg::test::TanSky::from_keywords( k );

   // Error, in core pixels, of a core -> PixInsight pixel mapping against the exported WCS.
   auto mappingError = [&]( double x, double y, const std::function<DPoint( double, double )>& toPI ) {
      DPoint rd;
      if ( !md.ImageToCelestial( rd, toPI( x, y ) ) )
         return 1e30;
      double tx, ty;
      if ( !tan.sky_to_pixel( rd.x, rd.y, tx, ty ) )
         return 1e30;
      return std::hypot( tx - x, ty - y );
   };
   auto correct = [&]( double x, double y ) { return sky.CoreToPI( x, y ); };
   auto noHalf = [&]( double x, double y ) { return DPoint( x, y ); };                  // no centre shift
   auto flipped = [&]( double x, double y ) { return DPoint( x + 0.5, H - y - 0.5 ); }; // bottom-up rows

   Err central, reverse, full, eNoHalf, eFlipped, roundTrip;
   double sumDx = 0, sumDy = 0, sumSq = 0;
   const int N = 20;
   for ( int j = 0; j <= N; ++j )
      for ( int i = 0; i <= N; ++i )
      {
         // central 50% of the field
         const double x = 0.25*(W - 1) + 0.5*(W - 1)*i/N;
         const double y = 0.25*(H - 1) + 0.5*(H - 1)*j/N;
         double ra, dec, tx, ty;
         if ( sky.pixel_to_sky( x, y, ra, dec ) && tan.sky_to_pixel( ra, dec, tx, ty ) )
            { central.Add( std::hypot( tx - x, ty - y ) ); sumDx += tx - x; sumDy += ty - y; sumSq += (tx - x)*(tx - x) + (ty - y)*(ty - y); }
         else
            central.Add( 1e30 );
         double ra2, dec2, px, py;
         if ( tan.pixel_to_sky( x, y, ra2, dec2 ) && sky.sky_to_pixel( ra2, dec2, px, py ) )
            reverse.Add( std::hypot( px - x, py - y ) );
         else
            reverse.Add( 1e30 );
         if ( sky.pixel_to_sky( x, y, ra, dec ) && sky.sky_to_pixel( ra, dec, px, py ) )
            roundTrip.Add( std::hypot( px - x, py - y ) );
         else
            roundTrip.Add( 1e30 );
         eNoHalf.Add( mappingError( x, y, noHalf ) );
         eFlipped.Add( mappingError( x, y, flipped ) );
         // whole field, corners included (linear WCS vs the spline solution: informational)
         full.Add( mappingError( (W - 1)*double( i )/N, (H - 1)*double( j )/N, correct ) );
      }

   // ImageBridge: core row y is PixInsight row y (FITS TOP-DOWN, as astropy read it); ToPcl
   // inverts ToCore exactly for a float32 image.
   const gdg::Image core = ImageBridge::ToCore( image );
   size_type bridgeMismatch = 0, roundTripMismatch = 0, checked = 0;
   const bool f32 = image.IsFloatSample() && image.BitsPerSample() == 32;
   if ( core.width != W || core.height != H || core.channels != image->NumberOfNominalChannels() || !f32 )
      bridgeMismatch = 1;
   else
   {
      const pcl::Image& src = static_cast<const pcl::Image&>( *image );
      for ( int c = 0; c < core.channels; ++c )
         for ( int y = 0; y < H; y += 7 )
            for ( int x = 0; x < W; x += 5, ++checked )
               if ( core.at( x, y, c ) != double( src( x, y, c ) ) )
                  ++bridgeMismatch;
      pcl::Image back;
      ImageBridge::ToPcl( core, back );
      for ( int c = 0; c < core.channels; ++c )
         for ( int y = 0; y < H; ++y )
            for ( int x = 0; x < W; ++x )
               if ( back( x, y, c ) != src( x, y, c ) )
                  ++roundTripMismatch;
   }

   return Field( "width", Num( W ) ) + "," + Field( "height", Num( H ) ) + "," +
          Field( "channels", Num( core.channels ) ) + "," +
          Field( "points", Num( central.n ) ) + "," +
          Field( "maxCentralPx", Num( central.max ) ) + "," + Field( "meanCentralPx", Num( central.Mean() ) ) + "," +
          Field( "meanOffsetXPx", Num( sumDx/central.n ) ) + "," + Field( "meanOffsetYPx", Num( sumDy/central.n ) ) + "," +
          Field( "rmsCentralPx", Num( std::sqrt( sumSq/central.n ) ) ) + "," +
          Field( "maxReversePx", Num( reverse.max ) ) + "," + Field( "maxRoundTripPx", Num( roundTrip.max ) ) + "," +
          Field( "maxFullPx", Num( full.max ) ) + "," +
          Field( "meanNoHalfPx", Num( eNoHalf.Mean() ) ) + "," +
          Field( "minFlippedPx", Num( [&] { double m = 1e300; for ( int j = 0; j <= N; ++j ) for ( int i = 0; i <= N; ++i ) { const double x = 0.25*(W - 1) + 0.5*(W - 1)*i/N, y = 0.25*(H - 1) + 0.5*(H - 1)*j/N; m = std::min( m, mappingError( x, y, flipped ) ); } return m; }() ) ) + "," +
          Field( "maxFlippedPx", Num( eFlipped.max ) ) + "," +
          Field( "bridgeChecked", Num( double( checked ) ) ) + "," +
          Field( "bridgeMismatches", Num( double( bridgeMismatch ) ) ) + "," +
          Field( "bridgeRoundTripMismatches", Num( double( roundTripMismatch ) ) ) + "," +
          Field( "crpix1", Num( k[ "CRPIX1" ] ) ) + "," + Field( "crpix2", Num( k[ "CRPIX2" ] ) );
}

} // namespace pcl
