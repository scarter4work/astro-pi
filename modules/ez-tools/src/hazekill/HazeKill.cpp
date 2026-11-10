// EZTools -- EZHazeKill engine (PCL-free).
// Copyright and licence: see src/EZToolsLicense.h.

#include "hazekill/HazeKill.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace eztools::hazekill
{

namespace
{

// Median of a[0..n-1], reordering a: the mean of the two middle values for an even n (numpy's
// definition). As the script's hkMedianInPlace: select the upper middle, then the lower middle is
// the maximum of the left partition.
double MedianInPlace( double* a, size_t n )
{
   size_t h = n >> 1;
   std::nth_element( a, a + h, a + n );
   double v = a[h];
   if ( n & 1 )
      return v;
   double lo = *std::max_element( a, a + h );
   return 0.5 * ( lo + v );
}

double Median( std::vector<double> a )
{
   return MedianInPlace( a.data(), a.size() );
}

// numpy.percentile, method "linear".
double Percentile( std::vector<double> a, double q )
{
   std::sort( a.begin(), a.end() );
   double pos = q / 100 * double( a.size() - 1 );
   size_t i0 = size_t( std::floor( pos ) );
   double f = pos - double( i0 );
   return ( i0 + 1 < a.size() ) ? a[i0] + f * ( a[i0 + 1] - a[i0] ) : a[i0];
}

// Samples are read as double(x) * scale: 1 for floating point, 1/max for integers (PixInsight's
// FromSample), tile by tile, so no whole-image converted copy is ever made.
template <typename T>
SkyEstimate MeasureSkyT( const std::vector<const T*>& channels, int width, int height, double scale = 1.0 )
{
   SkyEstimate e;
   if ( channels.empty() || width <= 0 || height <= 0 )
   {
      e.error = "the image is empty";
      return e;
   }
   const size_t nc = channels.size();
   const size_t plane = size_t( width ) * size_t( height );
   if constexpr ( std::is_floating_point_v<T> )
      for ( size_t c = 0; c < nc; ++c )
      {
         const T* p = channels[c];
         for ( size_t i = 0; i < plane; ++i )
            if ( !std::isfinite( p[i] ) )
            {
               e.error = "the image contains NaN or infinite samples";
               return e;
            }
      }

   // Tiles: partial right/bottom tiles are dropped; an image smaller than one tile in either
   // direction is measured as a single tile.
   int tw = kTile, th = kTile;
   if ( width < tw || height < th )
   {
      tw = width;
      th = height;
   }
   const int nx = width / tw, ny = height / th;
   const size_t count = size_t( nx ) * size_t( ny );
   const size_t n = size_t( tw ) * size_t( th );
   std::vector<std::vector<double>> med( nc, std::vector<double>( count ) ), mad( nc, std::vector<double>( count ) );
   std::vector<double> buf( n );
   for ( int ty = 0, t = 0; ty < ny; ++ty )
      for ( int tx = 0; tx < nx; ++tx, ++t )
         for ( size_t c = 0; c < nc; ++c )
         {
            const T* p = channels[c];
            size_t k = 0;
            for ( int y = ty * th; y < ty * th + th; ++y )
            {
               const T* row = p + size_t( y ) * width + size_t( tx ) * tw;
               for ( int x = 0; x < tw; ++x )
                  buf[k++] = double( row[x] ) * scale;
            }
            double m = MedianInPlace( buf.data(), n );
            for ( size_t i = 0; i < n; ++i )
               buf[i] = std::fabs( buf[i] - m );
            med[c][t] = m;
            mad[c][t] = 1.4826 * MedianInPlace( buf.data(), n );
         }

   // Tile luminance = mean of the channel medians; the darkest kSkyPercentile % are the sky.
   std::vector<double> L( count );
   for ( size_t t = 0; t < count; ++t )
   {
      double sum = 0;
      for ( size_t c = 0; c < nc; ++c )
         sum += med[c][t];
      L[t] = sum / double( nc );
   }
   const double cut = Percentile( L, kSkyPercentile );
   std::vector<size_t> sel;
   for ( size_t t = 0; t < count; ++t )
      if ( L[t] <= cut )
         sel.push_back( t );

   for ( size_t c = 0; c < nc; ++c )
   {
      std::vector<double> a, b;
      for ( size_t t : sel )
      {
         a.push_back( med[c][t] );
         b.push_back( mad[c][t] );
      }
      e.sky.push_back( Median( a ) );
      e.sigma.push_back( Median( b ) );
   }
   e.tiles = int( count );
   e.skyTiles = int( sel.size() );
   e.ok = true;
   return e;
}

template <typename T>
void ApplyT( const std::vector<T*>& channels, size_t n, const Model& m )
{
   for ( size_t c = 0; c < channels.size() && c < m.offset.size(); ++c )
   {
      T* p = channels[c];
      const double off = m.offset[c];
      for ( size_t i = 0; i < n; ++i )
         p[i] = T( ApplySample( double( p[i] ), off, m.eps ) );
   }
}

std::string Format( const char* fmt, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
std::string Format( const char* fmt, ... )
{
   char buf[512];
   va_list ap;
   va_start( ap, fmt );
   std::vsnprintf( buf, sizeof( buf ), fmt, ap );
   va_end( ap );
   return buf;
}

} // namespace

SkyEstimate MeasureSky( const std::vector<const double*>& channels, int width, int height )
{
   return MeasureSkyT( channels, width, height );
}

SkyEstimate MeasureSky( const std::vector<const float*>& channels, int width, int height )
{
   return MeasureSkyT( channels, width, height );
}

SkyEstimate MeasureSky( const std::vector<const uint8_t*>& channels, int width, int height )
{
   return MeasureSkyT( channels, width, height, 1.0/255 );
}

SkyEstimate MeasureSky( const std::vector<const uint16_t*>& channels, int width, int height )
{
   return MeasureSkyT( channels, width, height, 1.0/65535 );
}

SkyEstimate MeasureSky( const std::vector<const uint32_t*>& channels, int width, int height )
{
   return MeasureSkyT( channels, width, height, 1.0/4294967295.0 );
}

Model BuildModel( const SkyEstimate& e, const Settings& s )
{
   Model m;
   m.settings = s;
   m.sky = e.sky;
   m.sigma = e.sigma;
   m.tiles = e.tiles;
   m.skyTiles = e.skyTiles;
   const size_t nc = e.sky.size();
   if ( nc == 0 )
      return m;
   double skyL = 0, sigMax = 0;
   for ( size_t c = 0; c < nc; ++c )
   {
      skyL += e.sky[c];
      sigMax = std::max( sigMax, e.sigma[c] );
   }
   skyL /= double( nc );
   m.skyL = skyL;
   m.sigmaMax = sigMax;
   m.floorLevel = kFloorSigma * sigMax;
   m.tEff = std::max( s.target, m.floorLevel );
   m.ped = std::max( 0.0, skyL - m.tEff );            // never raise the pedestal
   for ( size_t c = 0; c < nc; ++c )
   {
      m.cast.push_back( e.sky[c] - skyL );
      m.offset.push_back( s.neutralize * m.cast[c] + s.amount * m.ped );
   }
   m.eps = std::max( 1.0e-6, kToeSigma * sigMax );
   m.stretched = sigMax > kStretchedNoise * skyL;
   return m;
}

double Toe( double u, double eps )
{
   return ( u >= eps ) ? u : eps * std::exp( u / eps - 1.0 );
}

void Apply( const std::vector<float*>& channels, size_t n, const Model& m )
{
   ApplyT( channels, n, m );
}

void Apply( const std::vector<double*>& channels, size_t n, const Model& m )
{
   ApplyT( channels, n, m );
}

std::string Report( const Model& r )
{
   static const char* rgb[] = { "R", "G", "B" };
   const size_t nc = r.sky.size();
   std::string out = Format( "Sky from the darkest %d%% of %d tiles (%d tiles):", int( kSkyPercentile ), r.tiles,
                             r.skyTiles );
   for ( size_t c = 0; c < nc; ++c )
      out += Format( "\n   %s: sky %.5f   sigma %.5f   cast %+.5f   subtract %+.5f", ( nc == 3 ) ? rgb[c] : "K",
                     r.sky[c], r.sigma[c], r.cast[c], r.offset[c] );
   out += Format( "\n   sky level %.5f, target %.5f, %.0f-sigma noise floor %.5f", r.skyL, r.settings.target,
                  kFloorSigma, r.floorLevel );
   if ( r.floorLevel > r.settings.target )
      out += Format( "\n   The noise floor (%.5f) is above the target: the sky stays at or above it.", r.floorLevel );
   if ( r.ped <= 0 )
      out += "\n   The sky is already at or below the target: the pedestal is left as it is (never raised).";
   else
      out += Format( "\n   Pedestal above the target: %.5f, removing %.0f%% of it (Amount %.2f).", r.ped,
                     100 * r.settings.amount, r.settings.amount );
   out += Format( "\n   Color cast removal: %.0f%% (Neutralize %.2f).", 100 * r.settings.neutralize,
                  r.settings.neutralize );
   if ( r.stretched )
      out += Format( "\nNote: the sky noise is %.1f%% of the sky level (more than %.0f%%), so this looks like "
                     "a stretched (non-linear) image. That is supported: the noise floor and the soft toe "
                     "keep the darkest sky from being crushed.",
                     100 * r.sigmaMax / r.skyL, 100 * kStretchedNoise );
   return out;
}

} // namespace eztools::hazekill
