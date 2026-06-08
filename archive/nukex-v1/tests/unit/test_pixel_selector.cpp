// NukeX Unit Tests - PixelSelector
// Regression tests for coordinate scaling, segmentation fallback, channel validation, edge detection

#include <catch_amalgamated.hpp>
#include "PixelSelector.h"
#include "RegionStatistics.h"
#include "helpers/TestHelpers.h"

using namespace pcl;

// ---------------------------------------------------------------------------
// Helper: build a flat segmentation map filled with a single class
// Returns (segMap, confMap) as flat vectors of size width*height
// ---------------------------------------------------------------------------
static std::pair<std::vector<int>, std::vector<float>>
MakeFlatSegMap( int width, int height, int regionClass, float confidence = 0.9f )
{
   size_t n = static_cast<size_t>( width ) * height;
   std::vector<int>   seg( n, regionClass );
   std::vector<float> conf( n, confidence );
   return { seg, conf };
}

// ---------------------------------------------------------------------------
// Helper: build a 2D segmentation map filled with a single class
// ---------------------------------------------------------------------------
static std::pair<std::vector<std::vector<int>>, std::vector<std::vector<float>>>
MakeFlatSegMap2D( int width, int height, int regionClass, float confidence = 0.9f )
{
   std::vector<std::vector<int>>   seg( height, std::vector<int>( width, regionClass ) );
   std::vector<std::vector<float>> conf( height, std::vector<float>( width, confidence ) );
   return { seg, conf };
}

// ===========================================================================
// 1. Coordinate scaling regression tests
// ===========================================================================

TEST_CASE( "PixelSelector coordinate scaling: seg 1024 -> image 4096", "[unit][pixelselector][scaling]" )
{
   PixelSelector ps;

   const int segW = 1024, segH = 1024;
   const int imgW = 4096, imgH = 4096;

   // Fill entire seg map with BrightExtended (class 3)
   auto [seg, conf] = MakeFlatSegMap( segW, segH,
                                       static_cast<int>( RegionClass::BrightExtended ) );

   ps.SetImageDimensions( imgW, imgH );
   ps.SetSegmentation( seg, conf, segW, segH );

   SECTION( "Center pixel maps correctly" )
   {
      // Image center (2048, 2048) should map into the seg map and return BrightExtended
      SpatialContext ctx = ps.GetSpatialContext( imgW / 2, imgH / 2 );
      REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::BrightExtended );
   }

   SECTION( "Pixel beyond seg dimensions returns correct class, not Background" )
   {
      // x=3000 is well beyond segW=1024, but within imgW=4096
      // After scaling: sx = 3000 * 1023 / 4095 ~= 749, which is valid
      SpatialContext ctx = ps.GetSpatialContext( 3000, 3000 );
      REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::BrightExtended );
   }

   SECTION( "Last image pixel (4095,4095) maps to last seg pixel" )
   {
      SpatialContext ctx = ps.GetSpatialContext( imgW - 1, imgH - 1 );
      REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::BrightExtended );
   }

   SECTION( "First image pixel (0,0) maps to first seg pixel" )
   {
      SpatialContext ctx = ps.GetSpatialContext( 0, 0 );
      REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::BrightExtended );
   }

   SECTION( "All corners of the image return correct class" )
   {
      // Top-left
      SpatialContext tl = ps.GetSpatialContext( 0, 0 );
      CHECK( static_cast<RegionClass>( tl.centerClass ) == RegionClass::BrightExtended );

      // Top-right
      SpatialContext tr = ps.GetSpatialContext( imgW - 1, 0 );
      CHECK( static_cast<RegionClass>( tr.centerClass ) == RegionClass::BrightExtended );

      // Bottom-left
      SpatialContext bl = ps.GetSpatialContext( 0, imgH - 1 );
      CHECK( static_cast<RegionClass>( bl.centerClass ) == RegionClass::BrightExtended );

      // Bottom-right
      SpatialContext br = ps.GetSpatialContext( imgW - 1, imgH - 1 );
      CHECK( static_cast<RegionClass>( br.centerClass ) == RegionClass::BrightExtended );
   }
}

TEST_CASE( "PixelSelector coordinate scaling: non-square seg map", "[unit][pixelselector][scaling]" )
{
   PixelSelector ps;

   const int segW = 512, segH = 256;
   const int imgW = 4096, imgH = 2048;

   // Fill with StarHalo (class 6)
   auto [seg, conf] = MakeFlatSegMap( segW, segH,
                                       static_cast<int>( RegionClass::StarHalo ) );

   ps.SetImageDimensions( imgW, imgH );
   ps.SetSegmentation( seg, conf, segW, segH );

   // Mid-image point: should scale and hit StarHalo
   SpatialContext ctx = ps.GetSpatialContext( 2000, 1000 );
   REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::StarHalo );
}

TEST_CASE( "PixelSelector coordinate scaling: seg map with two regions", "[unit][pixelselector][scaling]" )
{
   // Seg map: left half = Background, right half = BrightCompact
   const int segW = 100, segH = 100;
   const int imgW = 1000, imgH = 1000;

   std::vector<int>   seg( segW * segH );
   std::vector<float> conf( segW * segH, 0.9f );

   for ( int y = 0; y < segH; ++y )
      for ( int x = 0; x < segW; ++x )
      {
         size_t idx = static_cast<size_t>( y ) * segW + x;
         seg[idx] = ( x < segW / 2 )
            ? static_cast<int>( RegionClass::Background )
            : static_cast<int>( RegionClass::BrightCompact );
      }

   PixelSelector ps;
   ps.SetImageDimensions( imgW, imgH );
   ps.SetSegmentation( seg, conf, segW, segH );

   // Image pixel at x=100 -> sx ~= 100*99/999 ~= 9 (left half -> Background)
   SpatialContext left = ps.GetSpatialContext( 100, 500 );
   CHECK( static_cast<RegionClass>( left.centerClass ) == RegionClass::Background );

   // Image pixel at x=900 -> sx ~= 900*99/999 ~= 89 (right half -> BrightCompact)
   SpatialContext right = ps.GetSpatialContext( 900, 500 );
   CHECK( static_cast<RegionClass>( right.centerClass ) == RegionClass::BrightCompact );
}

// ===========================================================================
// 2. SetSegmentation sets image dimensions as fallback
// ===========================================================================

TEST_CASE( "SetSegmentation(flat) sets image dimensions when not previously set", "[unit][pixelselector][dimensions]" )
{
   PixelSelector ps;

   const int segW = 1024, segH = 1024;
   auto [seg, conf] = MakeFlatSegMap( segW, segH,
                                       static_cast<int>( RegionClass::Background ) );

   // Do NOT call SetImageDimensions beforehand
   ps.SetSegmentation( seg, conf, segW, segH );

   // Now querying at seg-boundary coordinates should work (image dims = seg dims)
   SpatialContext ctx = ps.GetSpatialContext( segW / 2, segH / 2 );
   REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::Background );
}

TEST_CASE( "SetSegmentation(2D) sets image dimensions when not previously set", "[unit][pixelselector][dimensions]" )
{
   PixelSelector ps;

   const int segW = 256, segH = 256;
   auto [seg2d, conf2d] = MakeFlatSegMap2D( segW, segH,
                                             static_cast<int>( RegionClass::FaintCompact ) );

   ps.SetSegmentation( seg2d, conf2d );

   SpatialContext ctx = ps.GetSpatialContext( 128, 128 );
   REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::FaintCompact );
}

TEST_CASE( "SetSegmentation(Image) sets image dimensions when not previously set", "[unit][pixelselector][dimensions]" )
{
   PixelSelector ps;

   const int segW = 64, segH = 64;

   // Create a 2-channel image (channel 0 = class, channel 1 = confidence)
   // For class BrightExtended (3), normalized = 3/7 ~= 0.4286
   Image segImg( segW, segH, ColorSpace::Gray );
   segImg.AllocateData( segW, segH, 2, ColorSpace::Gray );

   double classNorm = 3.0 / ( static_cast<int>( RegionClass::Count ) - 1 );
   for ( int y = 0; y < segH; ++y )
      for ( int x = 0; x < segW; ++x )
      {
         segImg.Pixel( x, y, 0 ) = classNorm;
         segImg.Pixel( x, y, 1 ) = 0.95;
      }

   ps.SetSegmentation( segImg );

   SpatialContext ctx = ps.GetSpatialContext( 32, 32 );
   REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::BrightExtended );
}

TEST_CASE( "SetImageDimensions before SetSegmentation is preserved", "[unit][pixelselector][dimensions]" )
{
   PixelSelector ps;

   // Set image dims FIRST
   ps.SetImageDimensions( 4096, 4096 );

   const int segW = 512, segH = 512;
   auto [seg, conf] = MakeFlatSegMap( segW, segH,
                                       static_cast<int>( RegionClass::Artifact ) );
   ps.SetSegmentation( seg, conf, segW, segH );

   // Image dims should remain 4096, not be overwritten to 512
   // Test: query at image coordinate (2048, 2048) should still work via scaling
   SpatialContext ctx = ps.GetSpatialContext( 2048, 2048 );
   REQUIRE( static_cast<RegionClass>( ctx.centerClass ) == RegionClass::Artifact );
}

// ===========================================================================
// 3. Channel validation: SelectPixel handles edge cases gracefully
// ===========================================================================

TEST_CASE( "SelectPixel with single-value stack returns that value", "[unit][pixelselector][channels]" )
{
   PixelSelector ps;

   // A stack with just one frame value
   std::vector<float> values = { 0.5f };
   PixelSelectionResult result = ps.SelectPixel( values, 0, 0 );

   REQUIRE( result.value == Catch::Approx( 0.5f ).margin( 0.01f ) );
   REQUIRE( result.totalFrames == 1 );
}

TEST_CASE( "SelectPixel with empty stack returns zero", "[unit][pixelselector][channels]" )
{
   PixelSelector ps;

   std::vector<float> values;
   PixelSelectionResult result = ps.SelectPixel( values, 0, 0 );

   REQUIRE( result.value == 0.0f );
   REQUIRE( result.totalFrames == 0 );
}

// ===========================================================================
// 4. Edge pixel detection after coordinate scaling fix
// ===========================================================================

TEST_CASE( "isEdgePixel is true only at actual image edges", "[unit][pixelselector][edge]" )
{
   PixelSelector ps;

   const int segW = 64, segH = 64;
   const int imgW = 4096, imgH = 4096;

   auto [seg, conf] = MakeFlatSegMap( segW, segH,
                                       static_cast<int>( RegionClass::Background ) );

   ps.SetImageDimensions( imgW, imgH );
   ps.SetSegmentation( seg, conf, segW, segH );

   SECTION( "Image corner (0,0) is edge" )
   {
      SpatialContext ctx = ps.GetSpatialContext( 0, 0 );
      REQUIRE( ctx.isEdgePixel == true );
   }

   SECTION( "Image bottom-right corner is edge" )
   {
      SpatialContext ctx = ps.GetSpatialContext( imgW - 1, imgH - 1 );
      REQUIRE( ctx.isEdgePixel == true );
   }

   SECTION( "Image center is NOT edge" )
   {
      SpatialContext ctx = ps.GetSpatialContext( imgW / 2, imgH / 2 );
      REQUIRE( ctx.isEdgePixel == false );
   }

   SECTION( "Pixel near seg boundary but far from image edge is NOT edge" )
   {
      // seg map is 64x64, so seg-pixel boundaries are at multiples of ~64
      // Image pixel x=64, y=64 maps to sx ~= 64*63/4095 ~= 0.98 -> sx=1
      // That's at seg boundary (sx=1 is not 0 or 63), but image is at (64,64),
      // which is NOT an edge of a 4096-wide image.
      // Current impl uses seg coords for edge check, so this tests the behavior.
      SpatialContext ctx = ps.GetSpatialContext( 64, 64 );
      // sx would be ~1, sy ~1, neither 0 nor 63, so isEdgePixel = false
      CHECK( ctx.isEdgePixel == false );
   }

   SECTION( "Pixel at image top edge (y=0) is edge" )
   {
      SpatialContext ctx = ps.GetSpatialContext( imgW / 2, 0 );
      REQUIRE( ctx.isEdgePixel == true );
   }

   SECTION( "Pixel at image left edge (x=0) is edge" )
   {
      SpatialContext ctx = ps.GetSpatialContext( 0, imgH / 2 );
      REQUIRE( ctx.isEdgePixel == true );
   }

   SECTION( "Pixel at image right edge is edge" )
   {
      SpatialContext ctx = ps.GetSpatialContext( imgW - 1, imgH / 2 );
      REQUIRE( ctx.isEdgePixel == true );
   }

   SECTION( "Pixel at image bottom edge is edge" )
   {
      SpatialContext ctx = ps.GetSpatialContext( imgW / 2, imgH - 1 );
      REQUIRE( ctx.isEdgePixel == true );
   }
}

// ===========================================================================
// 5. SelectPixel basic sanity
// ===========================================================================

TEST_CASE( "SelectPixel returns reasonable result for uniform stack", "[unit][pixelselector][select]" )
{
   PixelSelector ps;

   // Uniform stack: all values ~0.5
   std::vector<float> values = { 0.50f, 0.51f, 0.49f, 0.50f, 0.52f };
   PixelSelectionResult result = ps.SelectPixel( values, 100, 100 );

   // Selected value should be close to the median (~0.50)
   REQUIRE( result.value >= 0.48f );
   REQUIRE( result.value <= 0.53f );
   REQUIRE( result.totalFrames == 5 );
}

TEST_CASE( "SelectPixel rejects obvious outlier", "[unit][pixelselector][select]" )
{
   PixelSelector ps;

   // Stack with one clear outlier
   std::vector<float> values = { 0.50f, 0.51f, 0.49f, 0.50f, 0.05f };
   PixelSelectionResult result = ps.SelectPixel( values, 100, 100 );

   // Selected value should NOT be the outlier (0.05)
   REQUIRE( result.value >= 0.40f );
}

// ===========================================================================
// 6. No segmentation returns Background
// ===========================================================================

TEST_CASE( "GetSpatialContext without segmentation returns default (Background)", "[unit][pixelselector][noseg]" )
{
   PixelSelector ps;

   SpatialContext ctx = ps.GetSpatialContext( 500, 500 );
   REQUIRE( ctx.centerClass == 0 );  // Background
   REQUIRE( ctx.isEdgePixel == false );
   REQUIRE( ctx.numMatchingNeighbors == 0 );
}
