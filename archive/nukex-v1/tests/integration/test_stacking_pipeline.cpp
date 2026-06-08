// Integration tests for stacking pipeline with real FITS data
// Exercises FrameStreamer for median reference creation, normalization,
// and output dimension validation.
//
// Requires:
//   1. QNAP mount with M33 test data at test_data/M33_LPro_streaming/
//   2. PixInsight runtime (PCL API) - FrameStreamer uses pcl::FileFormat/Console
//      which require the PI runtime. When run standalone, tests SKIP gracefully.

#include <catch_amalgamated.hpp>
#include "../helpers/TestHelpers.h"
#include "FrameStreamer.h"

#include <pcl/String.h>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <cmath>
#include <numeric>

namespace fs = std::filesystem;

#define REQUIRE_INTEGRATION_ENV()                                        \
   do {                                                                  \
      if ( !NukeXTest::HasTestFITS() )                                   \
      {                                                                  \
         SKIP( "FITS test data not available" );                         \
      }                                                                  \
      if ( !NukeXTest::HasPCLRuntime() )                                            \
      {                                                                  \
         SKIP( "PixInsight PCL runtime not available (standalone mode)" ); \
      }                                                                  \
   } while(0)

// --------------------------------------------------------------------------

// Collect the first N .fit files from the test directory, sorted by name
static std::vector<pcl::String> GetStackTestPaths( int maxFiles = 5 )
{
   std::vector<std::string> paths;
   std::string dir = NukeXTest::TestDataDir() + "/M33_LPro_streaming";

   for ( const auto& entry : fs::directory_iterator( dir ) )
   {
      if ( entry.is_regular_file() || entry.is_symlink() )
      {
         std::string ext = entry.path().extension().string();
         std::transform( ext.begin(), ext.end(), ext.begin(), ::tolower );
         if ( ext == ".fit" || ext == ".fits" || ext == ".fts" )
            paths.push_back( entry.path().string() );
      }
   }

   std::sort( paths.begin(), paths.end() );
   if ( static_cast<int>( paths.size() ) > maxFiles )
      paths.resize( maxFiles );

   std::vector<pcl::String> result;
   result.reserve( paths.size() );
   for ( const auto& p : paths )
      result.push_back( pcl::String( p.c_str() ) );

   return result;
}

// --------------------------------------------------------------------------

TEST_CASE( "Stacking pipeline: median reference creation from 5 frames",
           "[integration]" )
{
   REQUIRE_INTEGRATION_ENV();

   auto paths = GetStackTestPaths( 5 );
   REQUIRE( paths.size() == 5 );

   pcl::FrameStreamer streamer;
   REQUIRE( streamer.Initialize( paths ) );

   int width    = streamer.Width();
   int height   = streamer.Height();
   int nFrames  = streamer.NumFrames();

   REQUIRE( width > 0 );
   REQUIRE( height > 0 );
   REQUIRE( nFrames == 5 );

   // Compute median reference for channel 0, first 20 rows only (speed)
   int testRows = std::min( 20, height );
   std::vector<std::vector<float>> medianImage( testRows,
                                                 std::vector<float>( width, 0.0f ) );

   std::vector<float> pixelStack( nFrames );

   for ( int y = 0; y < testRows; ++y )
   {
      std::vector<std::vector<float>> rowData;
      REQUIRE( streamer.ReadRow( y, 0, rowData ) );
      REQUIRE( static_cast<int>( rowData.size() ) == nFrames );

      for ( int x = 0; x < width; ++x )
      {
         // Gather the pixel stack across frames
         for ( int f = 0; f < nFrames; ++f )
            pixelStack[f] = rowData[f][x];

         // Compute median
         std::sort( pixelStack.begin(), pixelStack.end() );
         medianImage[y][x] = pixelStack[nFrames / 2];
      }
   }

   // Verify median values are valid (in [0, 1] for normalized FITS data)
   int validCount = 0;
   int totalPixels = testRows * width;

   for ( int y = 0; y < testRows; ++y )
   {
      for ( int x = 0; x < width; ++x )
      {
         float v = medianImage[y][x];
         if ( v >= 0.0f && v <= 1.0f )
            ++validCount;
      }
   }

   // All or nearly all pixels should be in [0,1]
   double validFraction = static_cast<double>( validCount ) / totalPixels;
   CHECK( validFraction > 0.99 );

   // The median should not be all zeros (real data has signal)
   bool hasSignal = false;
   for ( int y = 0; y < testRows && !hasSignal; ++y )
      for ( int x = 0; x < width && !hasSignal; ++x )
         if ( medianImage[y][x] > 0.001f )
            hasSignal = true;

   CHECK( hasSignal );

   INFO( "Median reference: " << testRows << " rows, " << width << " cols, "
         << validFraction * 100.0 << "% valid, hasSignal=" << hasSignal );
}

// --------------------------------------------------------------------------

TEST_CASE( "Stacking pipeline: normalization parameters are near unity",
           "[integration]" )
{
   REQUIRE_INTEGRATION_ENV();

   auto paths = GetStackTestPaths( 5 );
   REQUIRE( paths.size() == 5 );

   pcl::FrameStreamer streamer;
   REQUIRE( streamer.Initialize( paths ) );

   int width   = streamer.Width();
   int height  = streamer.Height();
   int nFrames = streamer.NumFrames();

   // Sample a grid of rows to compute per-frame mean and stddev (channel 0)
   // Use ~20 evenly spaced rows for speed
   int sampleRows = std::min( 20, height );
   int rowStep    = std::max( 1, height / sampleRows );

   // Accumulators per frame: sum
   std::vector<double> frameSum( nFrames, 0.0 );
   int sampledPixels = 0;

   for ( int yi = 0; yi < sampleRows; ++yi )
   {
      int y = yi * rowStep;
      if ( y >= height )
         break;

      std::vector<std::vector<float>> rowData;
      REQUIRE( streamer.ReadRow( y, 0, rowData ) );

      for ( int f = 0; f < nFrames; ++f )
      {
         for ( int x = 0; x < width; ++x )
         {
            double v = static_cast<double>( rowData[f][x] );
            frameSum[f] += v;
         }
      }
      sampledPixels += width;
   }

   REQUIRE( sampledPixels > 0 );

   // Compute per-frame mean
   std::vector<double> frameMean( nFrames );
   for ( int f = 0; f < nFrames; ++f )
      frameMean[f] = frameSum[f] / sampledPixels;

   // Compute global mean across frames
   double globalMean = 0.0;
   for ( int f = 0; f < nFrames; ++f )
      globalMean += frameMean[f];
   globalMean /= nFrames;

   REQUIRE( globalMean > 0.0 );

   // Normalization scale factor: globalMean / frameMean
   // For well-calibrated data from the same session, these should be near 1.0
   for ( int f = 0; f < nFrames; ++f )
   {
      double scale = globalMean / frameMean[f];
      double offset = globalMean - frameMean[f];

      INFO( "Frame " << f << ": mean=" << frameMean[f]
            << " scale=" << scale << " offset=" << offset );

      // Scale should be close to 1.0 (within 20% for same-session data)
      CHECK( scale > 0.8 );
      CHECK( scale < 1.2 );

      // Offset should be small relative to the mean
      CHECK( std::abs( offset ) < globalMean * 0.2 );
   }
}

// --------------------------------------------------------------------------

TEST_CASE( "Stacking pipeline: output dimensions match input frames",
           "[integration]" )
{
   REQUIRE_INTEGRATION_ENV();

   auto paths = GetStackTestPaths( 5 );
   REQUIRE( paths.size() == 5 );

   pcl::FrameStreamer streamer;
   REQUIRE( streamer.Initialize( paths ) );

   int width    = streamer.Width();
   int height   = streamer.Height();
   int channels = streamer.Channels();

   // Verify dimensions are consistent across all readable rows/channels
   // Read first row for each channel to confirm consistency
   for ( int c = 0; c < channels; ++c )
   {
      // Reset before each channel (ReadRow triggers internal reset on channel
      // change, but explicit reset ensures clean state)
      if ( c > 0 )
         REQUIRE( streamer.ResetAllFiles() );

      std::vector<std::vector<float>> rowData;
      REQUIRE( streamer.ReadRow( 0, c, rowData ) );
      REQUIRE( static_cast<int>( rowData.size() ) == streamer.NumFrames() );

      for ( int f = 0; f < streamer.NumFrames(); ++f )
      {
         CHECK( static_cast<int>( rowData[f].size() ) == width );
      }
   }

   // The output image would be width x height x channels
   CHECK( width > 0 );
   CHECK( height > 0 );
   CHECK( channels >= 1 );

   // Verify the dimensions are reasonable for astronomical data
   // (at least a few hundred pixels in each dimension)
   CHECK( width >= 100 );
   CHECK( height >= 100 );

   INFO( "Output dimensions: " << width << " x " << height
         << " x " << channels );
}
