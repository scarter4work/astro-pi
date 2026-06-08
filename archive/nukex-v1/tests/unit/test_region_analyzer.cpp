// NukeX Unit Tests - RegionAnalyzer
// Tests for QuickAnalyze, RegionStatistics construction and field access

#include <catch_amalgamated.hpp>
#include "RegionAnalyzer.h"
#include "RegionStatistics.h"

#include <pcl/Image.h>
#include <vector>
#include <cmath>

using namespace pcl;
using Catch::Approx;

// ---------------------------------------------------------------------------
// Helper: create an Image by writing pixels directly (avoids Image::Fill
// which calls StatusMonitor and segfaults outside PixInsight runtime)
// ---------------------------------------------------------------------------
static Image MakeConstantRGB( int w, int h, double value )
{
   Image img( w, h, ColorSpace::RGB );
   for ( int c = 0; c < img.NumberOfChannels(); ++c )
   {
      Image::sample* data = img.PixelData( c );
      size_t n = static_cast<size_t>( w ) * h;
      for ( size_t i = 0; i < n; ++i )
         data[i] = static_cast<Image::sample>( value );
   }
   return img;
}

static Image MakeGradientRGB( int w, int h )
{
   Image img( w, h, ColorSpace::RGB );
   for ( int c = 0; c < img.NumberOfChannels(); ++c )
      for ( int y = 0; y < h; ++y )
         for ( int x = 0; x < w; ++x )
         {
            double val = ( static_cast<double>( x ) / ( w - 1 ) +
                           static_cast<double>( y ) / ( h - 1 ) ) * 0.5;
            img.Pixel( x, y, c ) = val;
         }
   return img;
}

// ===========================================================================
// 1. QuickAnalyze: all-black image returns dynamicRange == 0
// ===========================================================================

TEST_CASE( "QuickAnalyze all-black image: dynamicRange is zero, not -Inf", "[unit][regionanalyzer][quickanalyze]" )
{
   RegionAnalyzer analyzer;

   // All-black RGB image (every pixel = 0.0)
   Image black = MakeConstantRGB( 64, 64, 0.0 );

   RegionAnalysisResult result = analyzer.QuickAnalyze( black );

   // The background stats should exist
   REQUIRE( result.regionStats.count( RegionClass::Background ) == 1 );

   const RegionStatistics& stats = result.regionStats.at( RegionClass::Background );

   SECTION( "dynamicRange is exactly 0.0" )
   {
      REQUIRE( stats.dynamicRange == 0.0 );
   }

   SECTION( "dynamicRange is finite (not -Inf or NaN)" )
   {
      REQUIRE( std::isfinite( stats.dynamicRange ) );
   }

   SECTION( "min and max are both 0.0" )
   {
      REQUIRE( stats.min == 0.0 );
      REQUIRE( stats.max == 0.0 );
   }

   SECTION( "mean and stdDev are 0.0" )
   {
      REQUIRE( stats.mean == 0.0 );
      REQUIRE( stats.stdDev == 0.0 );
   }

   SECTION( "SNR estimate is finite" )
   {
      REQUIRE( std::isfinite( stats.snrEstimate ) );
   }
}

TEST_CASE( "QuickAnalyze all-black image metadata is correct", "[unit][regionanalyzer][quickanalyze]" )
{
   RegionAnalyzer analyzer;
   Image black = MakeConstantRGB( 128, 64, 0.0 );

   RegionAnalysisResult result = analyzer.QuickAnalyze( black );

   REQUIRE( result.imageWidth == 128 );
   REQUIRE( result.imageHeight == 64 );
   REQUIRE( result.totalPixels == 128 * 64 );
   REQUIRE( result.isColor == true );  // RGB image
}

// ===========================================================================
// 2. QuickAnalyze: gradient image returns reasonable stats
// ===========================================================================

TEST_CASE( "QuickAnalyze gradient image: reasonable statistics", "[unit][regionanalyzer][quickanalyze]" )
{
   RegionAnalyzer analyzer;

   // Gradient from 0 to ~1 diagonally
   Image grad = MakeGradientRGB( 128, 128 );

   RegionAnalysisResult result = analyzer.QuickAnalyze( grad );

   REQUIRE( result.regionStats.count( RegionClass::Background ) == 1 );

   const RegionStatistics& stats = result.regionStats.at( RegionClass::Background );

   SECTION( "min is near 0" )
   {
      REQUIRE( stats.min >= 0.0 );
      REQUIRE( stats.min < 0.05 );
   }

   SECTION( "max is near 1" )
   {
      // Gradient max = (1 + 1) * 0.5 = 1.0 at bottom-right corner
      REQUIRE( stats.max >= 0.9 );
      REQUIRE( stats.max <= 1.0 );
   }

   SECTION( "mean is approximately 0.5" )
   {
      // For a linear gradient 0..1, mean should be roughly 0.5
      REQUIRE( stats.mean >= 0.3 );
      REQUIRE( stats.mean <= 0.7 );
   }

   SECTION( "stdDev is positive" )
   {
      REQUIRE( stats.stdDev > 0.0 );
   }

   SECTION( "dynamicRange is positive and finite" )
   {
      REQUIRE( stats.dynamicRange > 0.0 );
      REQUIRE( std::isfinite( stats.dynamicRange ) );
   }

   SECTION( "SNR estimate is positive and finite" )
   {
      REQUIRE( stats.snrEstimate > 0.0 );
      REQUIRE( std::isfinite( stats.snrEstimate ) );
   }
}

TEST_CASE( "QuickAnalyze constant non-zero image has zero stdDev", "[unit][regionanalyzer][quickanalyze]" )
{
   RegionAnalyzer analyzer;

   Image flat = MakeConstantRGB( 32, 32, 0.5 );
   RegionAnalysisResult result = analyzer.QuickAnalyze( flat );

   const RegionStatistics& stats = result.regionStats.at( RegionClass::Background );

   REQUIRE( stats.min == Approx( 0.5 ) );
   REQUIRE( stats.max == Approx( 0.5 ) );
   REQUIRE( stats.mean == Approx( 0.5 ) );
   REQUIRE( stats.stdDev == Approx( 0.0 ).margin( 1e-10 ) );
}

// ===========================================================================
// 3. RegionStatistics: construction and field access
// ===========================================================================

TEST_CASE( "RegionStatistics default construction", "[unit][regionstats]" )
{
   RegionStatistics stats;

   // Check documented defaults
   CHECK( stats.min == 0.0 );
   CHECK( stats.max == 1.0 );
   CHECK( stats.mean == 0.5 );
   CHECK( stats.median == 0.5 );
   CHECK( stats.stdDev == 0.1 );
   CHECK( stats.mad == 0.1 );
   CHECK( stats.dynamicRange == 1.0 );
   CHECK( stats.snrEstimate == 10.0 );
   CHECK( stats.regionClass == RegionClass::Background );
   CHECK( stats.pixelCount == 0 );
}

TEST_CASE( "RegionStatistics utility methods", "[unit][regionstats]" )
{
   RegionStatistics stats;
   stats.p25 = 0.3;
   stats.p75 = 0.7;
   stats.min = 0.1;
   stats.max = 0.9;
   stats.mean = 0.5;
   stats.stdDev = 0.15;

   CHECK( stats.InterquartileRange() == Approx( 0.4 ) );
   CHECK( stats.Range() == Approx( 0.8 ) );
   CHECK( stats.CoefficientOfVariation() == Approx( 0.3 ) );
}

TEST_CASE( "RegionStatistics classification helpers", "[unit][regionstats]" )
{
   RegionStatistics bright;
   bright.median = 0.6;
   CHECK( bright.IsBright() == true );
   CHECK( bright.IsFaint() == false );

   RegionStatistics faint;
   faint.median = 0.02;
   CHECK( faint.IsFaint() == true );
   CHECK( faint.IsBright() == false );

   RegionStatistics highSNR;
   highSNR.snrEstimate = 50.0;
   CHECK( highSNR.IsHighSNR() == true );
   CHECK( highSNR.IsLowSNR() == false );

   RegionStatistics lowSNR;
   lowSNR.snrEstimate = 2.0;
   CHECK( lowSNR.IsLowSNR() == true );
   CHECK( lowSNR.IsHighSNR() == false );

   RegionStatistics clipped;
   clipped.clippingPct = 5.0;
   CHECK( clipped.HasClipping() == true );

   RegionStatistics noClip;
   noClip.clippingPct = 0.0;
   CHECK( noClip.HasClipping() == false );

   RegionStatistics highDR;
   highDR.dynamicRange = 4.0;
   CHECK( highDR.IsHighDynamicRange() == true );
}

TEST_CASE( "RegionStatistics CoefficientOfVariation handles zero mean", "[unit][regionstats]" )
{
   RegionStatistics stats;
   stats.mean = 0.0;
   stats.stdDev = 0.1;

   // Should return 0 (guarded division), not Inf
   REQUIRE( stats.CoefficientOfVariation() == 0.0 );
}

// ===========================================================================
// 4. RegionClass enum utilities
// ===========================================================================

TEST_CASE( "RegionClass enum has 8 classes", "[unit][regionstats][enum]" )
{
   REQUIRE( static_cast<int>( RegionClass::Count ) == 8 );
}

TEST_CASE( "RegionClassToString round-trip", "[unit][regionstats][enum]" )
{
   CHECK( RegionClassToString( RegionClass::Background ) == "background" );
   CHECK( RegionClassToString( RegionClass::BrightCompact ) == "bright_compact" );
   CHECK( RegionClassToString( RegionClass::FaintCompact ) == "faint_compact" );
   CHECK( RegionClassToString( RegionClass::BrightExtended ) == "bright_extended" );
   CHECK( RegionClassToString( RegionClass::DarkExtended ) == "dark_extended" );
   CHECK( RegionClassToString( RegionClass::Artifact ) == "artifact" );
   CHECK( RegionClassToString( RegionClass::StarHalo ) == "star_halo" );
   CHECK( RegionClassToString( RegionClass::Vignette ) == "vignette" );
}

TEST_CASE( "IsStarRelatedClass identifies star classes", "[unit][regionstats][enum]" )
{
   CHECK( IsStarRelatedClass( RegionClass::BrightCompact ) == true );
   CHECK( IsStarRelatedClass( RegionClass::FaintCompact ) == true );
   CHECK( IsStarRelatedClass( RegionClass::StarHalo ) == true );
   CHECK( IsStarRelatedClass( RegionClass::Background ) == false );
   CHECK( IsStarRelatedClass( RegionClass::BrightExtended ) == false );
   CHECK( IsStarRelatedClass( RegionClass::DarkExtended ) == false );
   CHECK( IsStarRelatedClass( RegionClass::Artifact ) == false );
   CHECK( IsStarRelatedClass( RegionClass::Vignette ) == false );
}

TEST_CASE( "IsExtendedEmission identifies BrightExtended only", "[unit][regionstats][enum]" )
{
   CHECK( IsExtendedEmission( RegionClass::BrightExtended ) == true );
   CHECK( IsExtendedEmission( RegionClass::Background ) == false );
   CHECK( IsExtendedEmission( RegionClass::DarkExtended ) == false );
}

// ===========================================================================
// 5. RegionAnalyzer ClassifyRegion
// ===========================================================================

TEST_CASE( "ClassifyRegion categorizes based on median and SNR", "[unit][regionanalyzer][classify]" )
{
   RegionAnalyzer analyzer;

   SECTION( "Very bright -> BrightCompact" )
   {
      RegionStatistics stats;
      stats.median = 0.9;
      REQUIRE( analyzer.ClassifyRegion( stats ) == RegionClass::BrightCompact );
   }

   SECTION( "Low median -> Background" )
   {
      RegionStatistics stats;
      stats.median = 0.01;
      stats.min = 0.005;
      stats.max = 0.02;
      REQUIRE( analyzer.ClassifyRegion( stats ) == RegionClass::Background );
   }
}

// ===========================================================================
// 6. QuickAnalyze with mask pointer
// ===========================================================================

TEST_CASE( "QuickAnalyze accepts nullptr mask gracefully", "[unit][regionanalyzer][quickanalyze]" )
{
   RegionAnalyzer analyzer;
   Image img = MakeConstantRGB( 32, 32, 0.25 );

   // Explicit nullptr -- should not crash
   RegionAnalysisResult result = analyzer.QuickAnalyze( img, nullptr );

   REQUIRE( result.regionStats.count( RegionClass::Background ) == 1 );
   // Coverage should be 1.0 when no mask
   REQUIRE( result.regionCoverage.at( RegionClass::Background ) == Approx( 1.0 ) );
}

// ===========================================================================
// 7. QuickAnalyze via HistogramEngine vector path (no PCL Image)
// ===========================================================================

TEST_CASE( "HistogramEngine ComputeStatistics: all-zero vector gives dynamicRange 0", "[unit][regionanalyzer][quickanalyze]" )
{
   HistogramEngine engine;
   std::vector<double> values( 100, 0.0 );
   RegionStatistics stats = engine.ComputeStatistics( values );

   REQUIRE( stats.min == 0.0 );
   REQUIRE( stats.max == 0.0 );
   REQUIRE( stats.mean == 0.0 );
   REQUIRE( stats.stdDev == 0.0 );
   REQUIRE( std::isfinite( stats.dynamicRange ) );
}
