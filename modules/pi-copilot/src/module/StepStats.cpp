// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "StepStats.h"
#include "JourneyConstants.h"
#include "SafeFileWrite.h"
#include "ViewPreview.h"

#include <pcl/Bitmap.h>
#include <pcl/Exception.h>
#include <pcl/File.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

namespace pcl
{

double LaplacianNoiseSigma( const Image& img, int channel )
{
   const int w = img.Width(), h = img.Height();
   if ( w < 3 || h < 3 )
      return 0;
   std::vector<float> r;
   r.reserve( size_t( w - 2 )*size_t( h - 2 ) );
   const float* p = img.PixelData( channel );
   for ( int y = 1; y < h - 1; ++y )
      for ( int x = 1; x < w - 1; ++x )
      {
         const size_t i = size_t( y )*w + x;
         r.push_back( p[i] - 0.25f*(p[i - 1] + p[i + 1] + p[i - w] + p[i + w]) );
      }
   auto median = []( std::vector<float>& v )
   {
      const size_t m = v.size()/2;
      std::nth_element( v.begin(), v.begin() + m, v.end() );
      return double( v[m] );
   };
   const double med = median( r );
   for ( float& x : r )
      x = std::fabs( x - float( med ) );
   return PICopilotMadToSigma*median( r )/std::sqrt( 1.25 );
}

String WriteJourneyThumbnail( const Image& blockAveraged, const String& path )
{
   try
   {
      Image work( blockAveraged );   // StretchAndRender resamples/stretches in place
      Bitmap bmp = StretchAndRender( work, PICopilotJourneyThumbEdge );
      const String dir = File::ExtractDrive( path ) + File::ExtractDirectory( path );
      if ( !dir.IsEmpty() && !File::DirectoryExists( dir ) )
         File::CreateDirectory( dir );
      // Never Save() at the predictable final path: render privately, check
      // the JPEG magic, then atomic rename; a symlink target is refused (CWE-59).
      const String why = SafeRenderFile( path,
         [&bmp]( const String& temp ) { bmp.Save( temp, PICopilotJourneyThumbJpegQuality ); },
         SafeCheckJpeg );
      return why.IsEmpty() ? String() : "thumbnail " + why;
   }
   catch ( const pcl::Exception& x )
   {
      return "thumbnail " + path + ": " + x.Message();
   }
   catch ( const std::exception& x )
   {
      return "thumbnail " + path + ": " + String( x.what() );
   }
}

StepStatsResult ComputeStepStats( const View& view, const String& thumbnailPath )
{
   StepStatsResult r;
   const auto t0 = std::chrono::steady_clock::now();
   try
   {
      if ( view.IsNull() )
      {
         r.error = "no view for statistics";
         return r;
      }
      View v = view;
      if ( !v.CanRead() || !v.CanWrite() )
      {
         r.error = "view " + String( v.FullId() ) + " is busy (locked by a running process)";
         return r;
      }
      int k = 0, n = 0;
      const Image work = BlockAveragedCopy( v, PICopilotPreviewBlockEdge, k, PICopilotJourneyStatsRowStride, &n );
      r.blockFactor = k;
      r.rowStride = PICopilotJourneyStatsRowStride;
      r.samplesPerBlock = n;
      const Rect rc = work.Bounds();
      for ( int c = 0; c < work.NumberOfNominalChannels(); ++c )
      {
         ChannelStats s;
         s.channel = c;
         s.median = work.Median( rc, c, c );
         s.mad = work.MAD( s.median, rc, c, c );
         s.mean = work.Mean( rc, c, c );
         s.min = work.MinimumSampleValue( rc, c, c );
         s.max = work.MaximumSampleValue( rc, c, c );
         s.noise = LaplacianNoiseSigma( work, c )*std::sqrt( double( n ) );
         r.channels.push_back( s );
      }
      r.ok = true;
      if ( !thumbnailPath.IsEmpty() )
      {
         r.thumbnailError = WriteJourneyThumbnail( work, thumbnailPath );
         if ( r.thumbnailError.IsEmpty() )
            r.thumbnailPath = thumbnailPath;
      }
   }
   catch ( const pcl::Exception& x )
   {
      r.error = "statistics failed: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      r.error = "statistics failed: " + String( x.what() );
   }
   if ( !r.ok )
      r.channels.clear();
   r.elapsedMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
   return r;
}

} // namespace pcl
