// Integration tests for FrameStreamer with real FITS data
// Requires:
//   1. QNAP mount with M33 test data at test_data/M33_LPro_streaming/
//   2. PixInsight runtime (PCL API) - FrameStreamer uses pcl::FileFormat/Console
//      which require the PI runtime. Run via: PixInsight --run-script or as module test.
//      When run standalone, tests SKIP gracefully.

#include <catch_amalgamated.hpp>
#include "../helpers/TestHelpers.h"
#include "FrameStreamer.h"

#include <pcl/String.h>
#include <filesystem>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

// --------------------------------------------------------------------------

// Collect the first N .fit files from the test directory, sorted by name
static std::vector<pcl::String> GetTestFITSPaths( int maxFiles = 5 )
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

// Common precondition: FITS data available AND PCL runtime present
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

TEST_CASE( "FrameStreamer::Initialize opens FITS files and reads dimensions",
           "[integration]" )
{
   REQUIRE_INTEGRATION_ENV();

   auto paths = GetTestFITSPaths( 5 );
   REQUIRE( paths.size() == 5 );

   pcl::FrameStreamer streamer;
   REQUIRE( streamer.Initialize( paths ) );

   CHECK( streamer.NumFrames() == 5 );
   CHECK( streamer.Width() > 0 );
   CHECK( streamer.Height() > 0 );
   CHECK( streamer.Channels() >= 1 );

   INFO( "Dimensions: " << streamer.Width() << "x" << streamer.Height()
         << "x" << streamer.Channels() );
}

// --------------------------------------------------------------------------

TEST_CASE( "FrameStreamer::ReadRow reads row 0 with non-empty data",
           "[integration]" )
{
   REQUIRE_INTEGRATION_ENV();

   auto paths = GetTestFITSPaths( 5 );
   REQUIRE( !paths.empty() );

   pcl::FrameStreamer streamer;
   REQUIRE( streamer.Initialize( paths ) );

   std::vector<std::vector<float>> rowData;
   REQUIRE( streamer.ReadRow( 0, 0, rowData ) );

   // Should have one row per frame
   REQUIRE( rowData.size() == static_cast<size_t>( streamer.NumFrames() ) );

   for ( int f = 0; f < streamer.NumFrames(); ++f )
   {
      REQUIRE( static_cast<int>( rowData[f].size() ) == streamer.Width() );

      // At least some pixels should be non-zero (real astronomical data)
      bool hasNonZero = false;
      for ( float v : rowData[f] )
      {
         if ( v != 0.0f )
         {
            hasNonZero = true;
            break;
         }
      }
      CHECK( hasNonZero );
   }
}

// --------------------------------------------------------------------------

TEST_CASE( "FrameStreamer sequential read rows 0-10 yields varying data",
           "[integration]" )
{
   REQUIRE_INTEGRATION_ENV();

   auto paths = GetTestFITSPaths( 5 );
   REQUIRE( !paths.empty() );

   pcl::FrameStreamer streamer;
   REQUIRE( streamer.Initialize( paths ) );
   REQUIRE( streamer.Height() > 10 );

   // Read rows 0 through 10 and verify at least some rows differ
   std::vector<std::vector<float>> prevRow;
   int differentRowCount = 0;

   for ( int y = 0; y <= 10; ++y )
   {
      std::vector<std::vector<float>> rowData;
      REQUIRE( streamer.ReadRow( y, 0, rowData ) );
      REQUIRE( static_cast<int>( rowData[0].size() ) == streamer.Width() );

      if ( y > 0 )
      {
         // Compare frame 0's data between consecutive rows
         bool rowsDiffer = false;
         for ( int x = 0; x < streamer.Width(); ++x )
         {
            if ( rowData[0][x] != prevRow[0][x] )
            {
               rowsDiffer = true;
               break;
            }
         }
         if ( rowsDiffer )
            ++differentRowCount;
      }

      prevRow = rowData;
   }

   // Most consecutive row pairs should differ in real image data
   CHECK( differentRowCount >= 5 );
}

// --------------------------------------------------------------------------

TEST_CASE( "FrameStreamer::ResetAllFiles reproduces identical data",
           "[integration]" )
{
   REQUIRE_INTEGRATION_ENV();

   auto paths = GetTestFITSPaths( 5 );
   REQUIRE( !paths.empty() );

   pcl::FrameStreamer streamer;
   REQUIRE( streamer.Initialize( paths ) );

   // Read rows 0 and 5 before reset
   std::vector<std::vector<float>> row0_before, row5_before;
   REQUIRE( streamer.ReadRow( 0, 0, row0_before ) );
   REQUIRE( streamer.ReadRow( 5, 0, row5_before ) );

   // Reset and re-read
   REQUIRE( streamer.ResetAllFiles() );

   std::vector<std::vector<float>> row0_after, row5_after;
   REQUIRE( streamer.ReadRow( 0, 0, row0_after ) );
   REQUIRE( streamer.ReadRow( 5, 0, row5_after ) );

   // Data must be identical
   REQUIRE( row0_before.size() == row0_after.size() );
   for ( size_t f = 0; f < row0_before.size(); ++f )
   {
      REQUIRE( row0_before[f].size() == row0_after[f].size() );
      for ( size_t x = 0; x < row0_before[f].size(); ++x )
      {
         CHECK( row0_before[f][x] == row0_after[f][x] );
      }
   }

   REQUIRE( row5_before.size() == row5_after.size() );
   for ( size_t f = 0; f < row5_before.size(); ++f )
   {
      REQUIRE( row5_before[f].size() == row5_after[f].size() );
      for ( size_t x = 0; x < row5_before[f].size(); ++x )
      {
         CHECK( row5_before[f][x] == row5_after[f][x] );
      }
   }
}

// --------------------------------------------------------------------------

TEST_CASE( "FrameStreamer channel iteration yields different data per channel",
           "[integration]" )
{
   REQUIRE_INTEGRATION_ENV();

   auto paths = GetTestFITSPaths( 5 );
   REQUIRE( !paths.empty() );

   pcl::FrameStreamer streamer;
   REQUIRE( streamer.Initialize( paths ) );

   // This test only makes sense for multi-channel (RGB) data
   if ( streamer.Channels() < 3 )
   {
      SKIP( "Test data is mono; need RGB for channel iteration test" );
   }

   // Read a middle row for channels 0, 1, 2
   int testRow = streamer.Height() / 2;

   std::vector<std::vector<float>> ch0, ch1, ch2;
   REQUIRE( streamer.ReadRow( testRow, 0, ch0 ) );

   // ReadRow triggers an internal reset when the channel changes,
   // but we need to start from a clean read position for the new channel.
   REQUIRE( streamer.ResetAllFiles() );
   REQUIRE( streamer.ReadRow( testRow, 1, ch1 ) );

   REQUIRE( streamer.ResetAllFiles() );
   REQUIRE( streamer.ReadRow( testRow, 2, ch2 ) );

   // At least one pair of channels should differ for frame 0
   // (RGB channels carry different color information)
   int diffCount01 = 0;
   int diffCount02 = 0;
   int diffCount12 = 0;

   for ( int x = 0; x < streamer.Width(); ++x )
   {
      if ( ch0[0][x] != ch1[0][x] ) ++diffCount01;
      if ( ch0[0][x] != ch2[0][x] ) ++diffCount02;
      if ( ch1[0][x] != ch2[0][x] ) ++diffCount12;
   }

   // For real RGB astronomical data, channels should differ substantially
   bool channelsDiffer = ( diffCount01 > 0 ) || ( diffCount02 > 0 ) || ( diffCount12 > 0 );
   CHECK( channelsDiffer );

   INFO( "Channel differences (frame 0, row " << testRow << "): "
         << "R-G=" << diffCount01 << " R-B=" << diffCount02
         << " G-B=" << diffCount12 << " / " << streamer.Width() << " pixels" );
}
