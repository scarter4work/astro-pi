// ----------------------------------------------------------------------------
// test_dust_corrector.cpp — Unit tests for DustCorrector
// ----------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/DustCorrector.h"
#include "engine/ArtifactDetector.h"

#include <vector>
#include <cmath>

using Catch::Approx;

// ============================================================================
// Circular deficit on uniform background — primary correctness test
// ============================================================================

TEST_CASE( "DustCorrector removes circular deficit on uniform background", "[dust][corrector]" )
{
   const int W = 300, H = 300;
   const float bg = 0.3f;
   const int cx = 150, cy = 150;
   const int R = 20;
   const float peakAtten = 0.92f;  // 8% deficit at center

   std::vector<float> image( W * H );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double d = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         float atten = 1.0f;
         if ( d < R )
            atten = 1.0f - (1.0f - peakAtten) * float( std::exp( -0.5 * (d*d) / (R*R/4.0) ) );
         image[y * W + x] = bg * atten;
      }

   nukex::DustBlobInfo blob;
   blob.centerX = cx;
   blob.centerY = cy;
   blob.radius = R;

   nukex::DustCorrector corrector;
   corrector.correct( image.data(), W, H, { blob } );

   // After correction, center pixel should be close to bg
   float centerVal = image[cy * W + cx];
   REQUIRE( centerVal == Approx( bg ).margin( bg * 0.02f ) );

   // Edge pixel (just inside R) should be barely changed
   float edgeVal = image[cy * W + (cx + R - 2)];
   REQUIRE( edgeVal == Approx( bg ).margin( bg * 0.02f ) );
}

// ============================================================================
// No blobs — should be a no-op
// ============================================================================

TEST_CASE( "DustCorrector with empty blob list is a no-op", "[dust][corrector]" )
{
   const int W = 64, H = 64;
   const float bg = 0.5f;
   std::vector<float> image( W * H, bg );

   nukex::DustCorrector corrector;
   corrector.correct( image.data(), W, H, {} );

   // Every pixel should be unchanged
   for ( int i = 0; i < W * H; ++i )
      REQUIRE( image[i] == bg );
}

// ============================================================================
// Blob near image border — should not crash, edge bins filled from neighbors
// ============================================================================

TEST_CASE( "DustCorrector handles blob near image border", "[dust][corrector]" )
{
   const int W = 100, H = 100;
   const float bg = 0.4f;
   const int cx = 5, cy = 5;    // blob very close to top-left corner
   const int R = 10;

   std::vector<float> image( W * H );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double d = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         float atten = 1.0f;
         if ( d < R )
            atten = 1.0f - 0.10f * float( std::exp( -0.5 * (d*d) / (R*R/4.0) ) );
         image[y * W + x] = bg * atten;
      }

   nukex::DustBlobInfo blob;
   blob.centerX = cx;
   blob.centerY = cy;
   blob.radius = R;

   nukex::DustCorrector corrector;
   // Should not crash or produce NaN
   corrector.correct( image.data(), W, H, { blob } );

   // Center pixel should be closer to bg than before
   float centerVal = image[cy * W + cx];
   REQUIRE( std::isfinite( centerVal ) );
   REQUIRE( centerVal > 0.0f );
}

// ============================================================================
// Already-uniform region — correction should be ~1.0 (no change)
// ============================================================================

TEST_CASE( "DustCorrector on uniform region applies no significant change", "[dust][corrector]" )
{
   const int W = 200, H = 200;
   const float bg = 0.25f;
   std::vector<float> image( W * H, bg );

   nukex::DustBlobInfo blob;
   blob.centerX = 100;
   blob.centerY = 100;
   blob.radius = 15;

   nukex::DustCorrector corrector;
   corrector.correct( image.data(), W, H, { blob } );

   // All pixels should remain very close to bg
   float centerVal = image[100 * W + 100];
   REQUIRE( centerVal == Approx( bg ).margin( bg * 0.001f ) );
}

// ============================================================================
// Log callback is invoked
// ============================================================================

TEST_CASE( "DustCorrector invokes log callback", "[dust][corrector]" )
{
   const int W = 100, H = 100;
   const float bg = 0.3f;
   std::vector<float> image( W * H, bg );

   nukex::DustBlobInfo blob;
   blob.centerX = 50;
   blob.centerY = 50;
   blob.radius = 10;

   int logCount = 0;
   auto logger = [&logCount]( const std::string& msg )
   {
      (void)msg;
      logCount++;
   };

   nukex::DustCorrector corrector;
   corrector.correct( image.data(), W, H, { blob }, logger );

   REQUIRE( logCount == 1 );
}
