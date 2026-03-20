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

   // All frames have zero alignment offset (synthetic data, no shift)
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets( nSubs, { 0, 0 } );
   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );

   REQUIRE( result.blobs.size() >= 1 );
   REQUIRE( result.dustPixelCount > 0 );
   // The blob should be near the mote center
   bool foundNearCenter = false;
   for ( const auto& blob : result.blobs )
      if ( std::abs( blob.centerX - cx ) < 20 && std::abs( blob.centerY - cy ) < 20 )
         foundNearCenter = true;
   REQUIRE( foundNearCenter );
}

TEST_CASE( "detectDustSubcube rejects sky-fixed features (not sensor-fixed)", "[artifact][dust][subcube]" )
{
   // A dark blob at a FIXED ALIGNED position (sky feature, not sensor defect).
   // With alignment offsets, different frames read from different sensor positions
   // at this aligned location. In sensor space, the blob gets smeared and doesn't
   // form a compact circular component → should NOT be detected as dust.
   const int W = 200, H = 200;
   const int nSubs = 10;

   // Subcube: dark blob at fixed ALIGNED position (100, 100) in every frame.
   // This simulates a small dark nebula or galaxy feature.
   nukex::SubCube cube( nSubs, H, W );
   for ( size_t z = 0; z < static_cast<size_t>( nSubs ); ++z )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            double dist = std::sqrt( (x - 100.0) * (x - 100.0) + (y - 100.0) * (y - 100.0) );
            cube.setPixel( z, y, x, ( dist < 12.0 ) ? 0.45f : 0.5f );
         }

   std::vector<nukex::SubCube*> cubes = { &cube };
   std::vector<float> stacked( W * H, 0.5f );

   // Alignment offsets: each frame shifted by a different amount.
   // The sky feature stays at aligned (100,100) but maps to different sensor positions.
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets = {
      {0,0}, {10,0}, {20,0}, {30,0}, {40,0},
      {0,15}, {10,15}, {20,15}, {30,15}, {40,15}
   };

   nukex::ArtifactDetectorConfig cfg;
   cfg.dustMinDiameter    = 5;
   cfg.dustMaxDiameter    = 50;
   nukex::ArtifactDetector detector( cfg );

   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );

   // Should reject — the blob is smeared across sensor positions, not circular
   REQUIRE( result.blobs.empty() );
   REQUIRE( result.dustPixelCount == 0 );
}

// ============================================================================
// detectDustSubcube guard clause tests
// ============================================================================

TEST_CASE( "detectDustSubcube returns empty for small image", "[artifact][dust][subcube]" )
{
   const int W = 32, H = 32, nSubs = 5;
   std::vector<float> stacked( W * H, 0.5f );
   nukex::SubCube cube( nSubs, H, W );
   std::vector<nukex::SubCube*> cubes = { &cube };
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets( nSubs, { 0, 0 } );
   nukex::ArtifactDetector detector;
   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );
   REQUIRE( result.blobs.empty() );
   REQUIRE( result.dustPixelCount == 0 );
}

TEST_CASE( "detectDustSubcube returns empty for null cube", "[artifact][dust][subcube]" )
{
   const int W = 200, H = 200, nSubs = 5;
   std::vector<float> stacked( W * H, 0.5f );
   std::vector<nukex::SubCube*> cubes;
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets( nSubs, { 0, 0 } );
   nukex::ArtifactDetector detector;
   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );
   REQUIRE( result.blobs.empty() );
}

TEST_CASE( "detectDustSubcube returns empty for mismatched alignment count", "[artifact][dust][subcube]" )
{
   const int W = 200, H = 200, nSubs = 5;
   std::vector<float> stacked( W * H, 0.5f );
   nukex::SubCube cube( nSubs, H, W );
   std::vector<nukex::SubCube*> cubes = { &cube };
   // 3 offsets for 5 subs — mismatch
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets( 3, { 0, 0 } );
   nukex::ArtifactDetector detector;
   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );
   REQUIRE( result.blobs.empty() );
}

TEST_CASE( "detectDustSubcube returns empty for too few subs", "[artifact][dust][subcube]" )
{
   const int W = 200, H = 200, nSubs = 2;
   std::vector<float> stacked( W * H, 0.5f );
   nukex::SubCube cube( nSubs, H, W );
   std::vector<nukex::SubCube*> cubes = { &cube };
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets( nSubs, { 0, 0 } );
   nukex::ArtifactDetector detector;
   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );
   REQUIRE( result.blobs.empty() );
}

// ============================================================================
// detectDustSubcube with non-zero alignment offsets
// ============================================================================

TEST_CASE( "detectDustSubcube detects sensor-fixed mote with non-zero alignment offsets", "[artifact][dust][subcube]" )
{
   // Place a mote at a fixed SENSOR position. With alignment offsets,
   // each frame reads the mote at a different aligned position. In sensor
   // space (alignment reversed), the mote should appear sharp and circular.
   const int W = 300, H = 300;
   const int nSubs = 10;
   const int moteSensorX = 150, moteSensorY = 150;
   const int moteRadius = 15;
   const float bgVal = 0.5f;
   const float moteVal = 0.44f;  // 12% deficit

   // Alignment offsets — simulate dithering
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets = {
      {0,0}, {5,3}, {-3,7}, {10,0}, {-2,-5},
      {8,8}, {-6,4}, {3,-3}, {12,5}, {-4,10}
   };

   // Build subcube: for each frame z, the mote is at aligned position
   // (moteSensorX - dx, moteSensorY - dy) because alignment reversal does
   // sx + dx to get the aligned position.
   nukex::SubCube cube( nSubs, H, W );
   for ( size_t z = 0; z < static_cast<size_t>( nSubs ); ++z )
   {
      int moteAlignedX = moteSensorX - offsets[z].dx;
      int moteAlignedY = moteSensorY - offsets[z].dy;
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            double dist = std::sqrt( double( (x - moteAlignedX) * (x - moteAlignedX)
                                           + (y - moteAlignedY) * (y - moteAlignedY) ) );
            cube.setPixel( z, y, x, ( dist < moteRadius ) ? moteVal : bgVal );
         }
   }

   // Stacked image (not used by sensor-space detection, but required by API)
   std::vector<float> stacked( W * H, bgVal );

   std::vector<nukex::SubCube*> cubes = { &cube };

   nukex::ArtifactDetectorConfig cfg;
   cfg.dustMinDiameter = 10;
   cfg.dustMaxDiameter = 80;
   cfg.dustDetectionSigma = 3.0;
   nukex::ArtifactDetector detector( cfg );

   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );

   REQUIRE( result.blobs.size() >= 1 );
   REQUIRE( result.dustPixelCount > 0 );

   // The blob should be near the sensor-space mote center
   bool foundNearCenter = false;
   for ( const auto& blob : result.blobs )
      if ( std::abs( blob.centerX - moteSensorX ) < 25
           && std::abs( blob.centerY - moteSensorY ) < 25 )
         foundNearCenter = true;
   REQUIRE( foundNearCenter );

   // Correction map should be populated
   REQUIRE( result.correctionMap.size() == size_t( W * H ) );
   // Correction at mote center should be > 1.0 (undoing the attenuation)
   float centerCorrection = result.correctionMap[moteSensorY * W + moteSensorX];
   REQUIRE( centerCorrection > 1.0f );
}

// ============================================================================
// Radial extent tracing with Gaussian-profile mote
// ============================================================================

TEST_CASE( "detectDustSubcube expands mask for Gaussian-profile mote", "[artifact][dust][subcube]" )
{
   // A mote with gradual Gaussian falloff (not a hard circle). The detected
   // core should be smaller than the full extent. The radial extent tracing
   // should expand the mask beyond the core.
   // Image must be >> flatKernel (201px) for self-flat to work.
   const int W = 500, H = 500;
   const int nSubs = 10;
   const int cx = 250, cy = 250;
   const double moteSigma = 15.0;   // Gaussian sigma in pixels
   const double peakDeficit = 0.15;  // 15% attenuation at center
   const float bgVal = 0.5f;

   nukex::SubCube cube( nSubs, H, W );
   for ( size_t z = 0; z < static_cast<size_t>( nSubs ); ++z )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            double dist = std::sqrt( double( (x - cx) * (x - cx) + (y - cy) * (y - cy) ) );
            double attenuation = peakDeficit * std::exp( -0.5 * ( dist / moteSigma ) * ( dist / moteSigma ) );
            cube.setPixel( z, y, x, bgVal * ( 1.0f - static_cast<float>( attenuation ) ) );
         }

   std::vector<float> stacked( W * H, bgVal );
   std::vector<nukex::SubCube*> cubes = { &cube };
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets( nSubs, { 0, 0 } );

   nukex::ArtifactDetectorConfig cfg;
   cfg.dustMinDiameter = 5;
   cfg.dustMaxDiameter = 80;
   cfg.dustDetectionSigma = 3.0;
   nukex::ArtifactDetector detector( cfg );

   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );

   REQUIRE( result.blobs.size() >= 1 );

   // Find the blob near center
   const nukex::DustBlobInfo* moteBlob = nullptr;
   for ( const auto& blob : result.blobs )
      if ( std::abs( blob.centerX - cx ) < 30 && std::abs( blob.centerY - cy ) < 30 )
         moteBlob = &blob;
   REQUIRE( moteBlob != nullptr );

   // The mask radius should be expanded beyond the minimum (5px).
   // With sigma=15 and 12% peak, the extent at half-sigma (dustDetectionSigma/2 = 1.5)
   // should reach well beyond the core detection radius.
   REQUIRE( moteBlob->radius > 8.0 );

   // Correction map should taper: center correction > edge correction
   float centerCorr = result.correctionMap[cy * W + cx];
   int edgeX = cx + std::max( 1, static_cast<int>( moteBlob->radius * 0.8 ) );
   float edgeCorr = ( edgeX < W ) ? result.correctionMap[cy * W + edgeX] : 1.0f;
   REQUIRE( centerCorr > edgeCorr );
   REQUIRE( edgeCorr > 1.0f );
}

// ============================================================================
// Self-flat correction: mote embedded in vignetting gradient
// ============================================================================

TEST_CASE( "detectDustSubcube finds mote in vignetting gradient", "[artifact][dust][subcube]" )
{
   // A mote embedded in a vignetting gradient. The self-flat correction
   // (dividing by heavy smooth) should remove the gradient and isolate the mote.
   // Image must be >> flatKernel (201px) for self-flat to work.
   const int W = 500, H = 500;
   const int nSubs = 10;
   const int cx = 250, cy = 250;
   const int moteRadius = 15;

   nukex::SubCube cube( nSubs, H, W );
   for ( size_t z = 0; z < static_cast<size_t>( nSubs ); ++z )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            // Mild vignetting: brightness falls off radially from center (~6% at corners)
            double r = std::sqrt( double( (x - W/2) * (x - W/2) + (y - H/2) * (y - H/2) ) );
            double maxR = std::sqrt( double( W/2 * W/2 + H/2 * H/2 ) );
            double vignetting = 0.5 - 0.03 * ( r / maxR ) * ( r / maxR );

            // Add mote on top of vignetting
            double dist = std::sqrt( double( (x - cx) * (x - cx) + (y - cy) * (y - cy) ) );
            double val = vignetting;
            if ( dist < moteRadius )
               val *= 0.85;   // 15% mote attenuation
            cube.setPixel( z, y, x, static_cast<float>( val ) );
         }

   std::vector<float> stacked( W * H, 0.5f );
   std::vector<nukex::SubCube*> cubes = { &cube };
   std::vector<nukex::ArtifactDetector::AlignOffset> offsets( nSubs, { 0, 0 } );

   nukex::ArtifactDetectorConfig cfg;
   cfg.dustMinDiameter = 10;
   cfg.dustMaxDiameter = 80;
   cfg.dustDetectionSigma = 3.0;
   nukex::ArtifactDetector detector( cfg );

   auto result = detector.detectDustSubcube( stacked.data(), cubes, offsets, W, H );

   // Self-flat should remove the vignetting gradient, leaving only the mote
   REQUIRE( result.blobs.size() >= 1 );

   // Mote should be near the center
   bool foundNearCenter = false;
   for ( const auto& blob : result.blobs )
      if ( std::abs( blob.centerX - cx ) < 20 && std::abs( blob.centerY - cy ) < 20 )
         foundNearCenter = true;
   REQUIRE( foundNearCenter );

   // Should NOT have flagged massive pixel counts (would indicate vignetting leak).
   // On a 500x500 image with mild vignetting, some gradient residual is expected
   // from the self-flat kernel boundaries. The key assertion is that the mote
   // was found and there aren't hundreds of thousands of flagged pixels.
   REQUIRE( result.dustPixelCount < 50000 );
}
