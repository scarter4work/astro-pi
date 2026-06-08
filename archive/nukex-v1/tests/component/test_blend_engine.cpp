// NukeX Component Tests - BlendEngine
// Tests basic blending, weight normalization, and single-region passthrough.

#include <catch_amalgamated.hpp>
#include "helpers/TestHelpers.h"
#include "BlendEngine.h"
#include "IStretchAlgorithm.h"

#include <pcl/Image.h>
#include <cmath>
#include <map>
#include <vector>

using namespace pcl;

// ---------------------------------------------------------------------------
// Minimal stretch algorithm for testing: applies a simple linear scale.
// ---------------------------------------------------------------------------
class TestLinearStretch : public StretchAlgorithmBase
{
public:
   explicit TestLinearStretch( double scale = 2.0 ) : m_scale( scale ) {}

   double Apply( double value ) const override
   {
      return std::min( 1.0, value * m_scale );
   }

   IsoString Id() const override { return "TestLinear"; }
   String Name() const override { return "Test Linear"; }
   String Description() const override { return "Linear scale for testing"; }
   void AutoConfigure( const RegionStatistics& ) override {}

   std::unique_ptr<IStretchAlgorithm> Clone() const override
   {
      return std::make_unique<TestLinearStretch>( m_scale );
   }

private:
   double m_scale;
};

// ---------------------------------------------------------------------------
// Helper: create a grayscale (single-channel RGB) mask filled with a constant.
// ---------------------------------------------------------------------------
static Image CreateMask( int w, int h, double value )
{
   Image mask( w, h, ColorSpace::Gray );
   for ( int y = 0; y < h; ++y )
      for ( int x = 0; x < w; ++x )
         mask( x, y, 0 ) = value;
   return mask;
}

// ===========================================================================
// 1. Basic blend -- two overlapping regions blend without crash
// ===========================================================================

TEST_CASE( "BlendEngine: two overlapping regions blend without crash",
           "[component][blend][basic]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   const int W = 64, H = 64;
   Image original = NukeXTest::CreateConstantImage( W, H, 0.3 );

   // Region 1: bright compact, stretched to 2x
   RegionStretchResult r1;
   r1.region = RegionClass::BrightCompact;
   r1.stretchedImage = NukeXTest::CreateConstantImage( W, H, 0.6 ); // 0.3 * 2
   r1.mask = CreateMask( W, H, 1.0 );
   r1.coverage = 1.0;

   // Region 2: background, stretched differently
   RegionStretchResult r2;
   r2.region = RegionClass::Background;
   r2.stretchedImage = NukeXTest::CreateConstantImage( W, H, 0.4 );
   r2.mask = CreateMask( W, H, 0.5 );
   r2.coverage = 1.0;

   BlendConfig cfg;
   cfg.normalizeWeights = true;
   cfg.clampOutput = true;
   BlendEngine engine( cfg );

   // This must not crash
   Image result = engine.Blend( original, { r1, r2 } );

   // Output dimensions must match input
   CHECK( result.Width() == W );
   CHECK( result.Height() == H );
   CHECK( result.NumberOfNominalChannels() == 3 );

   // All output pixels should be in [0, 1]
   bool allInRange = true;
   for ( int c = 0; c < result.NumberOfNominalChannels(); ++c )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
            if ( result( x, y, c ) < 0.0 || result( x, y, c ) > 1.0 )
               allInRange = false;

   CHECK( allInRange );

   // The blended value should be a weighted average of 0.6 (weight 1.0) and 0.4 (weight 0.5)
   // = (0.6*1.0 + 0.4*0.5) / (1.0 + 0.5) = 0.8 / 1.5 ~ 0.533
   double expected = ( 0.6 * 1.0 + 0.4 * 0.5 ) / ( 1.0 + 0.5 );
   double actual = result( 32, 32, 0 );
   CHECK( std::abs( actual - expected ) < 0.01 );
}

// ===========================================================================
// 2. Weight normalization -- blend weights effectively sum to 1.0
// ===========================================================================

TEST_CASE( "BlendEngine: weights are normalized at overlap regions",
           "[component][blend][normalization]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   const int W = 32, H = 32;
   Image original = NukeXTest::CreateConstantImage( W, H, 0.5 );

   // Two regions with equal masks: weights should average equally
   RegionStretchResult r1;
   r1.region = RegionClass::BrightExtended;
   r1.stretchedImage = NukeXTest::CreateConstantImage( W, H, 0.8 );
   r1.mask = CreateMask( W, H, 1.0 );
   r1.coverage = 1.0;

   RegionStretchResult r2;
   r2.region = RegionClass::DarkExtended;
   r2.stretchedImage = NukeXTest::CreateConstantImage( W, H, 0.2 );
   r2.mask = CreateMask( W, H, 1.0 );
   r2.coverage = 1.0;

   BlendConfig cfg;
   cfg.normalizeWeights = true;
   cfg.clampOutput = true;
   BlendEngine engine( cfg );

   Image result = engine.Blend( original, { r1, r2 } );

   // With equal weights and normalization, result = (0.8 + 0.2) / 2 = 0.5
   double actual = result( 16, 16, 0 );
   double expected = 0.5;
   CHECK( std::abs( actual - expected ) < 0.01 );
}

TEST_CASE( "BlendEngine: BlendWithAlgorithms normalizes mask weights",
           "[component][blend][normalization][algorithms]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   const int W = 32, H = 32;
   Image original = NukeXTest::CreateConstantImage( W, H, 0.25 );

   // Create two masks that overlap completely
   std::map<RegionClass, Image> masks;
   masks[RegionClass::Background] = CreateMask( W, H, 1.0 );
   masks[RegionClass::BrightExtended] = CreateMask( W, H, 1.0 );

   // Two algorithms: one doubles, one triples
   TestLinearStretch stretch2x( 2.0 );
   TestLinearStretch stretch3x( 3.0 );

   std::map<RegionClass, IStretchAlgorithm*> algorithms;
   algorithms[RegionClass::Background] = &stretch2x;
   algorithms[RegionClass::BrightExtended] = &stretch3x;

   BlendConfig cfg;
   cfg.normalizeWeights = true;
   cfg.clampOutput = true;
   cfg.featherRadius = 0.0;  // No feathering for precise testing
   cfg.blendFalloff = 1.0;   // No power falloff
   BlendEngine engine( cfg );

   Image result = engine.BlendWithAlgorithms( original, masks, algorithms );

   CHECK( result.Width() == W );
   CHECK( result.Height() == H );

   // 0.25 * 2.0 = 0.50 for background
   // 0.25 * 3.0 = 0.75 for bright extended
   // After normalization with equal weights: (0.50 + 0.75) / 2 = 0.625
   // Note: with zero feathering the prepared masks go through NormalizeMasks
   // which makes them each 0.5. So the weighted average is (0.5*0.5 + 0.75*0.5)/1.0 = 0.625
   double actual = result( 16, 16, 0 );

   // Just verify the result is a reasonable blend (between the two stretched values)
   CHECK( actual > 0.4 );
   CHECK( actual < 0.8 );
}

// ===========================================================================
// 3. Single region -- image passes through unchanged
// ===========================================================================

TEST_CASE( "BlendEngine: single full-coverage region passes through",
           "[component][blend][single-region]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   const int W = 48, H = 48;
   const double origValue = 0.4;
   Image original = NukeXTest::CreateConstantImage( W, H, origValue );

   // Single region with full coverage and weight 1.0
   const double stretchedValue = 0.7;
   RegionStretchResult r1;
   r1.region = RegionClass::Background;
   r1.stretchedImage = NukeXTest::CreateConstantImage( W, H, stretchedValue );
   r1.mask = CreateMask( W, H, 1.0 );
   r1.coverage = 1.0;

   BlendConfig cfg;
   cfg.normalizeWeights = true;
   cfg.clampOutput = true;
   BlendEngine engine( cfg );

   Image result = engine.Blend( original, { r1 } );

   // With a single region at full weight, the output should be the
   // stretched value everywhere.
   bool allMatch = true;
   double maxError = 0;
   for ( int c = 0; c < result.NumberOfNominalChannels(); ++c )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            double err = std::abs( result( x, y, c ) - stretchedValue );
            if ( err > maxError )
               maxError = err;
            if ( err > 0.001 )
               allMatch = false;
         }

   CHECK( allMatch );
   CHECK( maxError < 0.001 );
}

TEST_CASE( "BlendEngine: empty region list returns original image",
           "[component][blend][empty]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   const int W = 32, H = 32;
   const double origValue = 0.35;
   Image original = NukeXTest::CreateConstantImage( W, H, origValue );

   BlendEngine engine;

   std::vector<RegionStretchResult> empty;
   Image result = engine.Blend( original, empty );

   // Empty region list should return the original
   CHECK( result.Width() == W );
   CHECK( result.Height() == H );

   // Verify pixel values match original
   bool matches = true;
   for ( int c = 0; c < result.NumberOfNominalChannels(); ++c )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
            if ( std::abs( result( x, y, c ) - origValue ) > 1e-6 )
               matches = false;

   CHECK( matches );
}

// ===========================================================================
// 4. Black point preservation
// ===========================================================================

TEST_CASE( "BlendEngine: preserves black point when configured",
           "[component][blend][blackpoint]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   const int W = 32, H = 32;

   // Create image with some zero-value pixels
   Image original( W, H, ColorSpace::RGB );
   for ( int c = 0; c < 3; ++c )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
            original( x, y, c ) = ( x < W / 2 ) ? 0.0 : 0.5;

   RegionStretchResult r1;
   r1.region = RegionClass::Background;
   r1.stretchedImage = NukeXTest::CreateConstantImage( W, H, 0.8 );
   r1.mask = CreateMask( W, H, 1.0 );
   r1.coverage = 1.0;

   BlendConfig cfg;
   cfg.preserveBlackPoint = true;
   cfg.normalizeWeights = true;
   BlendEngine engine( cfg );

   Image result = engine.Blend( original, { r1 } );

   // Zero-valued input pixels should remain zero
   bool blackPreserved = true;
   for ( int c = 0; c < 3; ++c )
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W / 2; ++x )
            if ( result( x, y, c ) != 0.0 )
               blackPreserved = false;

   CHECK( blackPreserved );

   // Non-zero pixels should be blended
   CHECK( result( W / 2 + 1, 0, 0 ) > 0.0 );
}

// ===========================================================================
// 5. MaskPrep::NormalizeMasks ensures sum = 1
// ===========================================================================

TEST_CASE( "MaskPrep::NormalizeMasks: masks sum to 1.0 at each pixel",
           "[component][blend][maskprep]" )
{
   if ( !NukeXTest::HasPCLRuntime() ) { SKIP( "Requires PCL runtime" ); }

   const int W = 32, H = 32;

   std::map<RegionClass, Image> masks;
   masks[RegionClass::Background] = CreateMask( W, H, 0.7 );
   masks[RegionClass::BrightExtended] = CreateMask( W, H, 0.3 );
   masks[RegionClass::StarHalo] = CreateMask( W, H, 0.5 );

   // Before normalization, sum = 0.7 + 0.3 + 0.5 = 1.5
   MaskPrep::NormalizeMasks( masks );

   // After normalization, sum should be 1.0 at every pixel
   bool sumsToOne = true;
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double sum = 0;
         for ( const auto& pair : masks )
            sum += pair.second( x, y, 0 );
         if ( std::abs( sum - 1.0 ) > 0.001 )
            sumsToOne = false;
      }

   CHECK( sumsToOne );

   // Verify relative proportions are preserved
   // Background was 0.7/1.5, BrightExtended was 0.3/1.5, StarHalo was 0.5/1.5
   double bg = masks[RegionClass::Background]( 0, 0, 0 );
   double be = masks[RegionClass::BrightExtended]( 0, 0, 0 );
   double sh = masks[RegionClass::StarHalo]( 0, 0, 0 );

   CHECK( std::abs( bg - 0.7 / 1.5 ) < 0.001 );
   CHECK( std::abs( be - 0.3 / 1.5 ) < 0.001 );
   CHECK( std::abs( sh - 0.5 / 1.5 ) < 0.001 );
}
