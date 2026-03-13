#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/ArtifactDetector.h"
#include <vector>
#include <cmath>

TEST_CASE( "TrailDetector detects bright diagonal line", "[artifact][trail]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.1f );
   // Draw bright diagonal line from (10,10) to (190,190), ~3px wide
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double dist = std::abs( x - y ) / std::sqrt( 2.0 );
         if ( dist < 1.5 )
            image[y * W + x] = 0.8f;
      }

   nukex::ArtifactDetectorConfig config;
   config.trailDilateRadius = 3.0;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectTrails( image.data(), W, H );

   REQUIRE( result.trailPixelCount > 0 );
   REQUIRE( result.mask[100 * W + 100] == 1 ); // center of diagonal
   REQUIRE( result.mask[0 * W + 100] == 0 );   // off-diagonal
}

TEST_CASE( "TrailDetector ignores faint background", "[artifact][trail]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.1f );
   nukex::ArtifactDetectorConfig config;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectTrails( image.data(), W, H );
   REQUIRE( result.trailPixelCount == 0 );
}
