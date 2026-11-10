// EZTools tests -- the C++ twin of make_golden.py's synthetic images (splitmix64 stream).
// Must produce exactly the samples make_golden.py fed to hk_lib: same stream order (channel-major,
// row-major), same libm calls (log, cos, sqrt, exp), same expressions.

#ifndef EZTOOLS_TEST_SYNTH_H
#define EZTOOLS_TEST_SYNTH_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace eztools::test
{

struct SplitMix64
{
   uint64_t s;
   explicit SplitMix64( uint64_t seed ) : s( seed ) {}
   uint64_t Next()
   {
      s += 0x9E3779B97F4A7C15ull;
      uint64_t z = s;
      z = ( z ^ ( z >> 30 ) ) * 0xBF58476D1CE4E5B9ull;
      z = ( z ^ ( z >> 27 ) ) * 0x94D049BB133111EBull;
      return z ^ ( z >> 31 );
   }
   double Uniform() { return double( Next() >> 11 ) * 0x1.0p-53; }
   double Gauss()
   {
      double u1 = Uniform();
      double u2 = Uniform();
      return std::sqrt( -2.0 * std::log( 1.0 - u1 ) ) * std::cos( 2.0 * M_PI * u2 );
   }
};

// Planar image: data[c][y*width + x].
struct SynthImage
{
   int width = 0, height = 0;
   std::vector<std::vector<double>> data;
};

// kind: "noise" (sky_c + sigma*g + blob), "constant" (sky_c), "clipped" (0 w.p. 0.6, else 0.05*u).
inline SynthImage Synth( uint64_t seed, int w, int h, const std::vector<double>& sky, double sigma, double blob,
                         const std::string& kind, double blobSigma = 60.0 )
{
   SplitMix64 rng( seed );
   SynthImage im;
   im.width = w;
   im.height = h;
   im.data.assign( sky.size(), std::vector<double>( size_t( w ) * h ) );
   double cx = 0.5 * w, cy = 0.5 * h;
   for ( size_t c = 0; c < sky.size(); ++c )
      for ( int y = 0; y < h; ++y )
         for ( int x = 0; x < w; ++x )
         {
            double v;
            if ( kind == "constant" )
               v = sky[c];
            else if ( kind == "clipped" )
            {
               double u = rng.Uniform();
               v = ( u < 0.6 ) ? 0.0 : 0.05 * rng.Uniform();
            }
            else
            {
               v = sky[c] + sigma * rng.Gauss();
               if ( blob != 0 )
               {
                  double d2 = ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy );
                  v += blob * std::exp( -d2 / ( 2.0 * blobSigma * blobSigma ) );
               }
            }
            im.data[c][size_t( y ) * w + x] = v;
         }
   return im;
}

} // namespace eztools::test

#endif
