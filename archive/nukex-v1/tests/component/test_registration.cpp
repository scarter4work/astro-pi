// NukeX Component Tests - FrameRegistration
// Tests the star detection, triangle matching, transform computation,
// near-identity skip, and RGBA luminance handling.

#include <catch_amalgamated.hpp>
#include "helpers/TestHelpers.h"
#include "FrameRegistration.h"

#include <pcl/Image.h>
#include <cmath>
#include <vector>

using namespace pcl;

// ---------------------------------------------------------------------------
// Helper: create an image with multiple synthetic stars at known positions.
// Each star is a Gaussian PSF added to a constant background.
// ---------------------------------------------------------------------------
static Image CreateMultiStarImage( int w, int h,
                                   const std::vector<std::pair<int,int>>& positions,
                                   double radius, double peak, double background = 0.05 )
{
   Image img( w, h, ColorSpace::RGB );
   img.Fill( background );

   for ( int c = 0; c < img.NumberOfNominalChannels(); ++c )
      for ( const auto& pos : positions )
         for ( int y = 0; y < h; ++y )
            for ( int x = 0; x < w; ++x )
            {
               double dx = x - pos.first;
               double dy = y - pos.second;
               double r2 = dx * dx + dy * dy;
               double sigma2 = radius * radius;
               double val = peak * std::exp( -r2 / ( 2.0 * sigma2 ) );
               double cur = img.Pixel( x, y, c );
               img.Pixel( x, y, c ) = std::min( 1.0, cur + val );
            }

   return img;
}

// ---------------------------------------------------------------------------
// Helper: shift an image by integer (dx, dy) pixels using direct pixel copy.
// Pixels that fall outside the frame are set to background.
// ---------------------------------------------------------------------------
static Image ShiftImage( const Image& src, int dx, int dy, double background = 0.05 )
{
   int w = src.Width();
   int h = src.Height();
   int channels = src.NumberOfNominalChannels();

   Image shifted( w, h, src.ColorSpace() );
   shifted.Fill( background );

   for ( int c = 0; c < channels; ++c )
      for ( int y = 0; y < h; ++y )
         for ( int x = 0; x < w; ++x )
         {
            int sx = x - dx;
            int sy = y - dy;
            if ( sx >= 0 && sx < w && sy >= 0 && sy < h )
               shifted.Pixel( x, y, c ) = src.Pixel( sx, sy, c );
         }

   return shifted;
}

// ===========================================================================
// 1. Identity registration
// ===========================================================================

TEST_CASE( "FrameRegistration: identity registration recovers zero displacement",
           "[component][registration][identity]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   // Place several bright stars spread across the field so triangle matching
   // has enough material to work with.
   std::vector<std::pair<int,int>> starPositions = {
      {60, 60}, {180, 50}, {100, 180}, {200, 150}, {150, 100},
      {40, 140}, {210, 200}, {130, 40}, {80, 220}, {220, 80}
   };

   Image ref = CreateMultiStarImage( 256, 256, starPositions, 3.0, 0.9 );

   // Make a pixel-perfect copy
   Image target( ref );

   FrameRegistrationConfig cfg;
   cfg.sensitivity = 0.7;
   cfg.enablePhaseCorrelation = false; // test triangle path only
   cfg.nearIdentityPx = 0.5;

   FrameRegistration reg( cfg );

   auto refStars = reg.DetectStarsInFrame( ref, cfg.maxStars );
   REQUIRE( refStars.size() >= 3 );

   auto refTriangles = reg.BuildTriangles( refStars, cfg.triangleStars, cfg.maxTriangles );
   REQUIRE( !refTriangles.empty() );

   FrameRegistrationResult result = reg.RegisterFrame( target, refStars, refTriangles, 1 );

   REQUIRE( result.success );

   // For an identical frame the transform must be near-identity
   CHECK( result.skippedNearIdentity );
   CHECK( std::abs( result.dx ) < 1.0 );
   CHECK( std::abs( result.dy ) < 1.0 );
   CHECK( std::abs( result.rotationDeg ) < 0.1 );
   CHECK( std::abs( result.scale - 1.0 ) < 0.01 );
}

// ===========================================================================
// 2. Known translation recovery
// ===========================================================================

TEST_CASE( "FrameRegistration: recovers known integer translation",
           "[component][registration][translation]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   std::vector<std::pair<int,int>> starPositions = {
      {60, 60}, {180, 50}, {100, 180}, {200, 150}, {150, 100},
      {40, 140}, {210, 200}, {130, 40}, {80, 220}, {220, 80}
   };

   Image ref = CreateMultiStarImage( 256, 256, starPositions, 3.0, 0.9 );

   // Shift by a known offset
   const int shiftX = 12;
   const int shiftY = -8;
   Image target = ShiftImage( ref, shiftX, shiftY );

   FrameRegistrationConfig cfg;
   cfg.sensitivity = 0.7;
   cfg.enablePhaseCorrelation = false;
   cfg.nearIdentityPx = 0.5;

   FrameRegistration reg( cfg );

   auto refStars = reg.DetectStarsInFrame( ref, cfg.maxStars );
   REQUIRE( refStars.size() >= 3 );

   auto refTriangles = reg.BuildTriangles( refStars, cfg.triangleStars, cfg.maxTriangles );
   REQUIRE( !refTriangles.empty() );

   FrameRegistrationResult result = reg.RegisterFrame( target, refStars, refTriangles, 1 );

   // Should succeed and detect the translation
   REQUIRE( result.success );
   CHECK_FALSE( result.skippedNearIdentity );

   // The recovered translation should be close to the applied shift.
   // The transform maps target->ref, so we expect tx ~ shiftX, ty ~ shiftY.
   double tolerance = 3.0; // pixels - triangle matching has some centroid noise
   CHECK( std::abs( result.dx - shiftX ) < tolerance );
   CHECK( std::abs( result.dy - shiftY ) < tolerance );
   CHECK( std::abs( result.rotationDeg ) < 0.5 );
   CHECK( std::abs( result.scale - 1.0 ) < 0.02 );
}

// ===========================================================================
// 3. NumberOfNominalChannels excludes alpha -- star detection on RGBA image
// ===========================================================================

TEST_CASE( "FrameRegistration: star detection uses NominalChannels (excludes alpha)",
           "[component][registration][rgba]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   // Create an RGB image with stars
   std::vector<std::pair<int,int>> starPositions = {
      {60, 60}, {180, 50}, {100, 180}, {200, 150}, {150, 100}
   };

   Image rgb = CreateMultiStarImage( 256, 256, starPositions, 3.0, 0.9, 0.05 );

   // Create an RGBA image: copy RGB data and add an alpha channel
   // PCL: ColorSpace::RGB has 3 nominal channels.
   // To add an alpha, we create a 4-channel image.
   Image rgba;
   rgba.AllocateData( 256, 256, 4, ColorSpace::RGB );  // 3 nominal + 1 alpha
   for ( int c = 0; c < 3; ++c )
      for ( int y = 0; y < 256; ++y )
         for ( int x = 0; x < 256; ++x )
            rgba.Pixel( x, y, c ) = rgb.Pixel( x, y, c );

   // Fill alpha channel with 1.0
   for ( int y = 0; y < 256; ++y )
      for ( int x = 0; x < 256; ++x )
         rgba.Pixel( x, y, 3 ) = 1.0;

   // NumberOfNominalChannels should be 3 (excluding alpha)
   CHECK( rgba.NumberOfNominalChannels() == 3 );
   CHECK( rgba.NumberOfChannels() == 4 );

   // Star detection should work correctly on RGBA without crash
   FrameRegistrationConfig cfg;
   cfg.sensitivity = 0.7;
   FrameRegistration reg( cfg );

   auto starsRGB = reg.DetectStarsInFrame( rgb, cfg.maxStars );
   auto starsRGBA = reg.DetectStarsInFrame( rgba, cfg.maxStars );

   // Both should detect roughly the same stars (RGBA alpha should be excluded
   // from the luminance computation because DetectStarsInFrame uses
   // NumberOfNominalChannels)
   REQUIRE( starsRGB.size() >= 3 );
   REQUIRE( starsRGBA.size() >= 3 );

   // Star counts should be the same since the pixel data is identical
   CHECK( starsRGB.size() == starsRGBA.size() );

   // Centroids should match closely
   if ( starsRGB.size() == starsRGBA.size() )
   {
      for ( size_t i = 0; i < starsRGB.size(); ++i )
      {
         CHECK( std::abs( starsRGB[i].x - starsRGBA[i].x ) < 0.5 );
         CHECK( std::abs( starsRGB[i].y - starsRGBA[i].y ) < 0.5 );
      }
   }
}

// ===========================================================================
// 4. Transform preservation -- rotation is accurately recovered
// ===========================================================================

TEST_CASE( "FrameRegistration: SimilarityTransform accurately encodes rotation and scale",
           "[component][registration][transform]" )
{
   // Verify that the SimilarityTransform struct preserves rotation and scale
   // through its a/b representation (no atan2 reconstruction artifacts).

   SECTION( "Pure rotation 1 degree" )
   {
      double angleDeg = 1.0;
      double angleRad = angleDeg * 3.14159265358979323846 / 180.0;
      double scale = 1.0;

      SimilarityTransform T;
      T.a = scale * std::cos( angleRad );
      T.b = scale * std::sin( angleRad );
      T.tx = 0;
      T.ty = 0;

      CHECK( std::abs( T.RotationDeg() - angleDeg ) < 1e-6 );
      CHECK( std::abs( T.Scale() - 1.0 ) < 1e-10 );
   }

   SECTION( "Rotation with small scale change" )
   {
      double angleDeg = 0.3;
      double angleRad = angleDeg * 3.14159265358979323846 / 180.0;
      double scale = 1.002;

      SimilarityTransform T;
      T.a = scale * std::cos( angleRad );
      T.b = scale * std::sin( angleRad );
      T.tx = 5.0;
      T.ty = -3.0;

      CHECK( std::abs( T.RotationDeg() - angleDeg ) < 1e-4 );
      CHECK( std::abs( T.Scale() - scale ) < 1e-8 );
      CHECK( std::abs( T.TranslationPx() - std::sqrt( 25.0 + 9.0 ) ) < 1e-10 );
   }

   SECTION( "Apply and verify forward transform" )
   {
      double angleDeg = 2.0;
      double angleRad = angleDeg * 3.14159265358979323846 / 180.0;

      SimilarityTransform T;
      T.a = std::cos( angleRad );
      T.b = std::sin( angleRad );
      T.tx = 10.0;
      T.ty = -5.0;

      // Apply to a known point
      double xp, yp;
      T.Apply( 100.0, 0.0, xp, yp );

      // Expected: x' = cos(2)*100 + 10, y' = sin(2)*100 - 5
      double expected_x = std::cos( angleRad ) * 100.0 + 10.0;
      double expected_y = std::sin( angleRad ) * 100.0 - 5.0;

      CHECK( std::abs( xp - expected_x ) < 1e-8 );
      CHECK( std::abs( yp - expected_y ) < 1e-8 );
   }

   SECTION( "ComputeSimilarityTransform recovers known rotation from star pairs" )
   {
      // Create synthetic matched star pairs with a known rotation
      double angleDeg = 0.5;
      double angleRad = angleDeg * 3.14159265358979323846 / 180.0;
      double ca = std::cos( angleRad ), sa = std::sin( angleRad );
      double txExpected = 3.0, tyExpected = -2.0;

      std::vector<DetectedStar> refStars = {
         {100, 100, 1.0}, {200, 100, 1.0}, {150, 200, 1.0},
         {80, 180, 1.0},  {220, 160, 1.0}, {130, 60, 1.0}
      };

      // Target = T^{-1}(ref) -- the transform maps target->ref
      // So ref = T(target) means target = T^{-1}(ref)
      // For testing ComputeSimilarityTransform: we provide ref and target stars
      // and expect it to recover T.
      std::vector<DetectedStar> targetStars;
      for ( const auto& r : refStars )
      {
         // Inverse of T: x = (a*(rx-tx) + b*(ry-ty))/(a^2+b^2)
         double dx = r.x - txExpected;
         double dy = r.y - tyExpected;
         // For unit scale: inv is just rotate by -angle and subtract translation
         DetectedStar t;
         t.x = ca * dx + sa * dy;
         t.y = -sa * dx + ca * dy;
         t.flux = 1.0;
         targetStars.push_back( t );
      }

      std::vector<StarMatch> matches;
      for ( int i = 0; i < static_cast<int>( refStars.size() ); ++i )
      {
         StarMatch m;
         m.refIndex = i;
         m.targetIndex = i;
         m.residual = 0;
         matches.push_back( m );
      }

      FrameRegistration reg;
      SimilarityTransform T = reg.ComputeSimilarityTransform( refStars, targetStars, matches );

      CHECK( std::abs( T.RotationDeg() - angleDeg ) < 0.01 );
      CHECK( std::abs( T.Scale() - 1.0 ) < 0.001 );
      CHECK( std::abs( T.tx - txExpected ) < 0.1 );
      CHECK( std::abs( T.ty - tyExpected ) < 0.1 );
   }
}

// ===========================================================================
// 5. Near-identity skip -- sub-pixel transform skips resampling
// ===========================================================================

TEST_CASE( "FrameRegistration: near-identity transform skips resampling",
           "[component][registration][near-identity]" )
{
   SECTION( "IsNearIdentity returns true for sub-pixel displacement" )
   {
      SimilarityTransform T;
      T.a = 1.0;
      T.b = 0.0;
      T.tx = 0.2;
      T.ty = 0.3;

      // TranslationPx = sqrt(0.04 + 0.09) ~ 0.36 < 0.5
      CHECK( T.IsNearIdentity( 0.5 ) );
   }

   SECTION( "IsNearIdentity returns false for multi-pixel displacement" )
   {
      SimilarityTransform T;
      T.a = 1.0;
      T.b = 0.0;
      T.tx = 2.0;
      T.ty = 1.0;

      CHECK_FALSE( T.IsNearIdentity( 0.5 ) );
   }

   SECTION( "IsNearIdentity returns false when rotation exceeds threshold" )
   {
      double angleDeg = 0.1; // > 0.01 threshold
      double angleRad = angleDeg * 3.14159265358979323846 / 180.0;

      SimilarityTransform T;
      T.a = std::cos( angleRad );
      T.b = std::sin( angleRad );
      T.tx = 0.0;
      T.ty = 0.0;

      CHECK_FALSE( T.IsNearIdentity( 0.5 ) );
   }

   SECTION( "Identical frames result in skippedNearIdentity=true via RegisterFrame" )
   {
      if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

      std::vector<std::pair<int,int>> starPositions = {
         {60, 60}, {180, 50}, {100, 180}, {200, 150}, {150, 100},
         {40, 140}, {210, 200}, {130, 40}, {80, 220}, {220, 80}
      };

      Image ref = CreateMultiStarImage( 256, 256, starPositions, 3.0, 0.9 );
      Image target( ref );

      FrameRegistrationConfig cfg;
      cfg.sensitivity = 0.7;
      cfg.enablePhaseCorrelation = false;
      cfg.nearIdentityPx = 0.5;

      FrameRegistration reg( cfg );
      auto refStars = reg.DetectStarsInFrame( ref, cfg.maxStars );
      auto refTri = reg.BuildTriangles( refStars, cfg.triangleStars, cfg.maxTriangles );

      FrameRegistrationResult result = reg.RegisterFrame( target, refStars, refTri, 1 );

      REQUIRE( result.success );
      CHECK( result.skippedNearIdentity );

      // Verify the frame was NOT modified (no resampling applied).
      // Compare a subset of pixels; they should be bit-identical to ref.
      bool unchanged = true;
      for ( int y = 50; y < 60; ++y )
         for ( int x = 50; x < 60; ++x )
            if ( target.Pixel( x, y, 0 ) != ref.Pixel( x, y, 0 ) )
               unchanged = false;

      CHECK( unchanged );
   }
}
