#pragma once

#include <catch_amalgamated.hpp>
#include <pcl/Image.h>
#include <pcl/String.h>
#include <vector>
#include <cmath>
#include <random>
#include <filesystem>
#include <csignal>
#include <csetjmp>

#include <pcl/Console.h>

namespace NukeXTest
{

// Create a small test image filled with a constant value
inline pcl::Image CreateConstantImage( int w, int h, double value )
{
   pcl::Image img( w, h, pcl::ColorSpace::RGB );
   img.Fill( value );
   return img;
}

// Create a gradient image (0 at top-left, 1 at bottom-right)
inline pcl::Image CreateGradientImage( int w, int h )
{
   pcl::Image img( w, h, pcl::ColorSpace::RGB );
   for ( int c = 0; c < img.NumberOfChannels(); ++c )
      for ( int y = 0; y < h; ++y )
         for ( int x = 0; x < w; ++x )
         {
            double val = ( static_cast<double>( x ) / ( w - 1 ) +
                           static_cast<double>( y ) / ( h - 1 ) ) * 0.5;
            img.Pixel( x, y, c ) = val;
         }
   return img;
}

// Create an image with a synthetic star at (cx, cy) with given radius and peak
inline pcl::Image CreateStarImage( int w, int h, int cx, int cy,
                                   double radius, double peak, double background = 0.1 )
{
   pcl::Image img( w, h, pcl::ColorSpace::RGB );
   img.Fill( background );
   for ( int c = 0; c < img.NumberOfChannels(); ++c )
      for ( int y = 0; y < h; ++y )
         for ( int x = 0; x < w; ++x )
         {
            double dx = x - cx;
            double dy = y - cy;
            double r2 = dx * dx + dy * dy;
            double sigma2 = radius * radius;
            double val = background + peak * std::exp( -r2 / ( 2.0 * sigma2 ) );
            img.Pixel( x, y, c ) = std::min( 1.0, val );
         }
   return img;
}

// Create a vector of float values for stack testing
inline std::vector<float> MakeStack( std::initializer_list<float> values )
{
   return std::vector<float>( values );
}

// Create a sorted copy
template <typename T>
inline std::vector<T> Sorted( std::vector<T> v )
{
   std::sort( v.begin(), v.end() );
   return v;
}

// Check that a value is in [lo, hi]
inline bool InRange( double val, double lo, double hi )
{
   return val >= lo && val <= hi;
}

// Path to test data directory
inline std::string TestDataDir()
{
   return "/home/scarter4work/projects/NukeX/test_data";
}

// Check if test FITS data is available (QNAP mounted)
inline bool HasTestFITS()
{
   return std::filesystem::exists( TestDataDir() + "/M33_LPro_streaming" );
}

// --------------------------------------------------------------------------
// PCL Runtime Detection
// --------------------------------------------------------------------------
// FrameStreamer and other PCL I/O classes (Console, FileFormat, etc.) require
// the PixInsight runtime API. When running as a standalone test binary outside
// PixInsight, these calls will SIGSEGV. This function probes once using a
// signal handler and caches the result across all translation units.

namespace detail
{
   inline jmp_buf& PclProbeJmpBuf()
   {
      static jmp_buf buf;
      return buf;
   }

   inline void PclProbeSignalHandler( int /*sig*/ )
   {
      longjmp( PclProbeJmpBuf(), 1 );
   }
} // namespace detail

inline bool HasPCLRuntime()
{
   // Cached result: -1 = untested, 0 = unavailable, 1 = available
   static int status = -1;
   if ( status >= 0 )
      return status == 1;

   struct sigaction sa{}, oldSA{}, oldBUS{};
   sa.sa_handler = detail::PclProbeSignalHandler;
   sigemptyset( &sa.sa_mask );
   sa.sa_flags = 0;

   sigaction( SIGSEGV, &sa, &oldSA );
   sigaction( SIGBUS, &sa, &oldBUS );

   bool available = false;
   if ( setjmp( detail::PclProbeJmpBuf() ) == 0 )
   {
      pcl::Console c;
      (void)c;
      available = true;
   }

   sigaction( SIGSEGV, &oldSA, nullptr );
   sigaction( SIGBUS, &oldBUS, nullptr );

   status = available ? 1 : 0;
   return available;
}

} // namespace NukeXTest
