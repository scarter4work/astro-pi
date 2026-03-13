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

// ============================================================================
// Dust detection tests
// ============================================================================

TEST_CASE( "DustDetector detects dark circular blob", "[artifact][dust]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.5f );
   // Dark circular dust mote at center, radius 20px
   int cx = 100, cy = 100, r = 20;
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double dist = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         if ( dist < r )
            image[y * W + x] = 0.3f;
      }

   nukex::ArtifactDetectorConfig config;
   config.dustMinDiameter = 10;
   config.dustMaxDiameter = 100;
   config.dustCircularityMin = 0.5;
   config.dustDetectionSigma = 1.5;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectDust( image.data(), W, H );

   REQUIRE( result.dustPixelCount > 0 );
   REQUIRE( result.blobs.size() == 1 );
   REQUIRE( result.mask[cy * W + cx] == 1 );
   REQUIRE( result.blobs[0].circularity > 0.8 );
}

TEST_CASE( "DustDetector ignores non-circular dark regions", "[artifact][dust]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.5f );
   // Long thin dark rectangle (NOT circular)
   for ( int y = 90; y < 110; ++y )
      for ( int x = 20; x < 180; ++x )
         image[y * W + x] = 0.3f;

   nukex::ArtifactDetectorConfig config;
   config.dustCircularityMin = 0.7;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectDust( image.data(), W, H );
   REQUIRE( result.blobs.empty() );
}

TEST_CASE( "DustDetector handles uniform image with no dust", "[artifact][dust]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.5f );

   nukex::ArtifactDetectorConfig config;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectDust( image.data(), W, H );

   REQUIRE( result.dustPixelCount == 0 );
   REQUIRE( result.blobs.empty() );
}

TEST_CASE( "DustDetector rejects blobs smaller than dustMinDiameter", "[artifact][dust]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.5f );
   // Tiny dark dot, radius 3px (diameter 6)
   int cx = 100, cy = 100, r = 3;
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double dist = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         if ( dist < r )
            image[y * W + x] = 0.3f;
      }

   nukex::ArtifactDetectorConfig config;
   config.dustMinDiameter = 20;   // minimum 20px diameter — blob is only ~6px
   config.dustDetectionSigma = 1.5;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectDust( image.data(), W, H );
   REQUIRE( result.blobs.empty() );
}

TEST_CASE( "DustDetector rejects blobs larger than dustMaxDiameter", "[artifact][dust]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H, 0.5f );
   // Large dark circle, radius 60px (diameter 120)
   int cx = 100, cy = 100, r = 60;
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double dist = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         if ( dist < r )
            image[y * W + x] = 0.3f;
      }

   nukex::ArtifactDetectorConfig config;
   config.dustMaxDiameter = 50;   // max 50px — blob is 120px
   config.dustDetectionSigma = 1.5;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectDust( image.data(), W, H );
   REQUIRE( result.blobs.empty() );
}

TEST_CASE( "DustDetector detects multiple dust motes", "[artifact][dust]" )
{
   const int W = 300, H = 300;
   std::vector<float> image( W * H, 0.5f );
   // Two dark circular dust motes
   struct Mote { int cx, cy, r; };
   Mote motes[] = { {80, 80, 15}, {220, 220, 18} };
   for ( auto& m : motes )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            double dist = std::sqrt( double((x-m.cx)*(x-m.cx) + (y-m.cy)*(y-m.cy)) );
            if ( dist < m.r )
               image[y * W + x] = 0.3f;
         }

   nukex::ArtifactDetectorConfig config;
   config.dustMinDiameter = 10;
   config.dustMaxDiameter = 100;
   config.dustCircularityMin = 0.5;
   config.dustDetectionSigma = 1.5;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectDust( image.data(), W, H );

   REQUIRE( result.blobs.size() == 2 );
   REQUIRE( result.dustPixelCount > 0 );
}

// ============================================================================
// Vignetting detection tests
// ============================================================================

TEST_CASE( "VignettingDetector detects radial brightness falloff", "[artifact][vignetting]" )
{
   const int W = 200, H = 200;
   std::vector<float> image( W * H );
   double cx = W / 2.0, cy = H / 2.0;
   double maxR = std::sqrt( cx*cx + cy*cy );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double r = std::sqrt( (x-cx)*(x-cx) + (y-cy)*(y-cy) ) / maxR;
         image[y * W + x] = float( 0.8 - 0.3 * r * r );
      }

   nukex::ArtifactDetectorConfig config;
   config.vignettingPolyOrder = 3;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectVignetting( image.data(), W, H, nullptr );

   float centerCorr = result.correctionMap[100 * W + 100];
   REQUIRE( centerCorr < 1.05f );

   float cornerCorr = result.correctionMap[0 * W + 0];
   REQUIRE( cornerCorr > 1.15f );

   // Corrected image should be more uniform
   float corrCenter = image[100 * W + 100] * centerCorr;
   float corrCorner = image[0 * W + 0] * cornerCorr;
   REQUIRE( std::abs(corrCenter - corrCorner) < 0.1f );
}

TEST_CASE( "VignettingDetector produces identity on flat image", "[artifact][vignetting]" )
{
   const int W = 100, H = 100;
   std::vector<float> image( W * H, 0.5f );
   nukex::ArtifactDetectorConfig config;
   nukex::ArtifactDetector detector( config );
   auto result = detector.detectVignetting( image.data(), W, H, nullptr );
   for ( float c : result.correctionMap )
      REQUIRE( c == Catch::Approx(1.0f).margin(0.02f) );
}

// ============================================================================
// Integration test — full detection pipeline
// ============================================================================

TEST_CASE( "Full detection pipeline runs without crash", "[artifact][integration]" )
{
   const int W = 100, H = 100;
   std::vector<float> image( W * H );

   // Image with vignetting gradient
   double cx = W/2.0, cy = H/2.0, maxR = std::sqrt(cx*cx + cy*cy);
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double r = std::sqrt((x-cx)*(x-cx) + (y-cy)*(y-cy)) / maxR;
         image[y * W + x] = float( 0.5 - 0.15 * r * r );
      }

   // Add circular dust mote at (30,30) with radius 10
   for ( int y = 20; y < 40; ++y )
      for ( int x = 20; x < 40; ++x )
      {
         double dist = std::sqrt( double((x-30)*(x-30) + (y-30)*(y-30)) );
         if ( dist < 10 )
            image[y * W + x] *= 0.6f;
      }

   // Add diagonal trail
   for ( int y = 0; y < H; ++y )
   {
      int x = y;
      if ( x >= 0 && x < W )
         image[y * W + x] = 0.9f;
   }

   nukex::ArtifactDetectorConfig config;
   nukex::ArtifactDetector detector( config );

   // NOTE: detectAll() takes 3 args (image, width, height) — no enable booleans
   auto result = detector.detectAll( image.data(), W, H );

   // Verify it runs without crash and produces valid masks
   REQUIRE( result.trails.mask.size() == size_t(W * H) );
   REQUIRE( result.dust.mask.size() == size_t(W * H) );
   REQUIRE( result.vignetting.correctionMap.size() == size_t(W * H) );
}
