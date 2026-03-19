#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/ArtifactDetector.h"
#include "engine/SubCube.h"
#include <vector>
#include <cmath>
#include <random>

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

TEST_CASE( "DustDetector rejects tiny blobs on noisy background", "[artifact][dust]" )
{
   // With difference-of-smoothing, a tiny blob on a noisy background
   // produces deficit below the sigma threshold and is correctly rejected.
   // Noise gives a realistic MAD so the threshold is meaningful.
   const int W = 200, H = 200;
   std::vector<float> image( W * H );
   std::mt19937 rng( 12345 );   // fixed seed for reproducibility
   std::normal_distribution<float> noise( 0.5f, 0.01f );
   for ( int i = 0; i < W * H; ++i )
      image[i] = std::max( 0.0f, std::min( 1.0f, noise( rng ) ) );

   // Tiny dark dot, radius 2px (diameter 4) with subtle deficit (3%)
   // The DoS deficit from this blob is below the 3-sigma noise threshold
   int cx = 100, cy = 100, r = 2;
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double dist = std::sqrt( double((x-cx)*(x-cx) + (y-cy)*(y-cy)) );
         if ( dist < r )
            image[y * W + x] = 0.485f;
      }

   nukex::ArtifactDetectorConfig config;
   config.dustMinDiameter = 10;
   config.dustMaxDiameter = 100;
   config.dustDetectionSigma = 3.0;   // 3-sigma threshold with realistic noise
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

// ============================================================================
// Subcube-verified dust detection tests
// ============================================================================

TEST_CASE( "detectDustSubcube verifies against subcube consistency", "[artifact][dust][subcube]" )
{
   // Synthetic image (background 0.5) with dust mote at center.
   // Image must be large enough for the scan ring (ringOuter=50).
   // Subcube frames all have the same depression plus small noise → dust.
   const int W = 200, H = 200;
   const int nSubs = 10;
   const int cx = 100, cy = 100;
   const int moteRadius = 12;

   // Stacked image with circular depression
   std::vector<float> stacked( W * H, 0.5f );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double dist = std::sqrt( (x - cx) * (x - cx) + (y - cy) * (y - cy) );
         if ( dist < moteRadius )
            stacked[y * W + x] = 0.45f;   // 10% darker
      }

   // Subcube: every frame has the same depression (consistent = dust)
   nukex::SubCube cube( nSubs, H, W );
   for ( size_t z = 0; z < static_cast<size_t>( nSubs ); ++z )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
            cube.setPixel( z, y, x, stacked[y * W + x] + 0.001f * ( float( z % 3 ) - 1.0f ) );

   std::vector<nukex::SubCube*> cubes = { &cube };

   nukex::ArtifactDetectorConfig cfg;
   cfg.dustMinDiameter    = 10;
   cfg.dustMaxDiameter    = 50;
   nukex::ArtifactDetector detector( cfg );

   auto result = detector.detectDustSubcube( stacked.data(), cubes, W, H );

   REQUIRE( result.blobs.size() >= 1 );
   REQUIRE( result.dustPixelCount > 0 );
   // The blob should be near the mote center
   bool foundNearCenter = false;
   for ( const auto& blob : result.blobs )
      if ( std::abs( blob.centerX - cx ) < 20 && std::abs( blob.centerY - cy ) < 20 )
         foundNearCenter = true;
   REQUIRE( foundNearCenter );
}

TEST_CASE( "detectDustSubcube rejects inconsistent blobs", "[artifact][dust][subcube]" )
{
   // Dark blob in subcube only appears in half the frames → median ratio ≈ 1.0
   // because the 5 frames without the blob pull the median above threshold.
   const int W = 200, H = 200;
   const int nSubs = 10;
   const int cx = 100, cy = 100;

   // Stacked image with a dark blob
   std::vector<float> stacked( W * H, 0.5f );
   for ( int y = 95; y < 105; ++y )
      for ( int x = 95; x < 105; ++x )
         stacked[y * W + x] = 0.45f;

   // Subcube: blob only in first 5 frames, flat background in last 5
   nukex::SubCube cube( nSubs, H, W );
   for ( size_t z = 0; z < static_cast<size_t>( nSubs ); ++z )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            if ( z < 5 )
               cube.setPixel( z, y, x, stacked[y * W + x] );
            else
               cube.setPixel( z, y, x, 0.5f );   // no depression in later frames
         }

   std::vector<nukex::SubCube*> cubes = { &cube };

   nukex::ArtifactDetectorConfig cfg;
   cfg.dustMinDiameter    = 5;
   cfg.dustMaxDiameter    = 50;
   nukex::ArtifactDetector detector( cfg );

   auto result = detector.detectDustSubcube( stacked.data(), cubes, W, H );

   // Should reject — median across frames is ~1.0 (inconsistent)
   REQUIRE( result.blobs.empty() );
   REQUIRE( result.dustPixelCount == 0 );
}
