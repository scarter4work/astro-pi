// NukeX Unit Tests - Stretch Algorithms
// Regression tests for all 12 stretch algorithms: boundaries, monotonicity, edge cases

#include <catch_amalgamated.hpp>
#include "helpers/TestHelpers.h"

#include "StretchLibrary.h"
#include "IStretchAlgorithm.h"

// Include all 12 algorithm headers for direct instantiation
#include "MTFStretch.h"
#include "HistogramStretch.h"
#include "GHStretch.h"
#include "ArcSinhStretch.h"
#include "LogStretch.h"
#include "LumptonStretch.h"
#include "RNCStretch.h"
#include "PhotometricStretch.h"
#include "OTSStretch.h"
#include "SASStretch.h"
#include "VeraluxStretch.h"
#include "StatisticalAutoStretch.h"

#include <vector>
#include <cmath>
#include <memory>

using namespace pcl;

// ============================================================================
// Helper: list of all 12 implemented algorithm types (excluding Auto which
// is a meta-selector that defaults to GHS)
// ============================================================================

static const std::vector<AlgorithmType> AllAlgorithmTypes = {
   AlgorithmType::MTF,
   AlgorithmType::Histogram,
   AlgorithmType::GHS,
   AlgorithmType::ArcSinh,
   AlgorithmType::Log,
   AlgorithmType::Lumpton,
   AlgorithmType::RNC,
   AlgorithmType::Photometric,
   AlgorithmType::OTS,
   AlgorithmType::SAS,
   AlgorithmType::Veralux,
   AlgorithmType::StatAuto
};

// Helper: create an algorithm from the library
static std::unique_ptr<IStretchAlgorithm> MakeAlgorithm( AlgorithmType type )
{
   return StretchLibrary::Instance().Create( type );
}

// Helper: get a display name for GENERATE labels
static std::string AlgoName( AlgorithmType type )
{
   return std::string( IsoString( StretchLibrary::TypeToName( type ) ).c_str() );
}

// ============================================================================
// Boundary: Apply(0.0) returns 0.0 (VeraluxStretch fix regression)
// ============================================================================

TEST_CASE( "Stretch boundary: Apply(0.0) returns 0.0 for all algorithms", "[unit][stretch]" )
{
   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         REQUIRE( algo != nullptr );

         double result = algo->Apply( 0.0 );
         // Must return exactly 0.0 or very close (numerical precision)
         REQUIRE( result == Catch::Approx( 0.0 ).margin( 1e-10 ) );
      }
   }
}

// ============================================================================
// Boundary: Apply(1.0) returns 1.0
// ============================================================================

TEST_CASE( "Stretch boundary: Apply(1.0) returns 1.0 for most algorithms", "[unit][stretch]" )
{
   // PhotometricStretch intentionally compresses highlights (uses log/asinh
   // transform that does not preserve 1.0 -> 1.0), so it is excluded.
   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      if ( type == AlgorithmType::Photometric )
         continue;

      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         REQUIRE( algo != nullptr );

         double result = algo->Apply( 1.0 );
         REQUIRE( result == Catch::Approx( 1.0 ).margin( 1e-10 ) );
      }
   }
}

TEST_CASE( "Stretch boundary: PhotometricStretch Apply(1.0) in [0,1]", "[unit][stretch]" )
{
   auto algo = MakeAlgorithm( AlgorithmType::Photometric );
   double result = algo->Apply( 1.0 );
   REQUIRE( result >= 0.0 );
   REQUIRE( result <= 1.0 );
}

// ============================================================================
// Output clamped to [0,1] for various inputs
// ============================================================================

TEST_CASE( "Stretch clamping: output in [0,1] for standard inputs", "[unit][stretch]" )
{
   const std::vector<double> testValues = {
      0.0, 0.001, 0.01, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5,
      0.6, 0.7, 0.8, 0.9, 0.95, 0.99, 0.999, 1.0
   };

   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         REQUIRE( algo != nullptr );

         for ( double input : testValues )
         {
            double result = algo->Apply( input );
            REQUIRE( result >= 0.0 );
            REQUIRE( result <= 1.0 );
         }
      }
   }
}

TEST_CASE( "Stretch clamping: edge values produce finite [0,1] output", "[unit][stretch]" )
{
   // Test values right at the boundary and slightly beyond
   const std::vector<double> edgeValues = { 0.0, 1e-15, 1e-10, 1e-5, 1.0 - 1e-10, 1.0 };

   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         for ( double v : edgeValues )
         {
            double result = algo->Apply( v );
            REQUIRE( std::isfinite( result ) );
            REQUIRE( result >= 0.0 );
            REQUIRE( result <= 1.0 );
         }
      }
   }
}

// ============================================================================
// Monotonicity: Apply(a) <= Apply(b) when a < b
// ============================================================================

TEST_CASE( "Stretch monotonicity: Apply(a) <= Apply(b) when a < b", "[unit][stretch]" )
{
   // Generate 100 evenly spaced test points
   std::vector<double> points;
   for ( int i = 0; i <= 100; ++i )
      points.push_back( static_cast<double>( i ) / 100.0 );

   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         REQUIRE( algo != nullptr );

         double prev = algo->Apply( points[0] );
         for ( size_t i = 1; i < points.size(); ++i )
         {
            double curr = algo->Apply( points[i] );
            // Allow tiny numerical tolerance for floating point
            REQUIRE( curr >= prev - 1e-10 );
            prev = curr;
         }
      }
   }
}

// ============================================================================
// All-black image: AutoConfigure with stats.median=0 doesn't crash
// ============================================================================

TEST_CASE( "AutoConfigure: all-black stats (median=0) does not crash", "[unit][stretch]" )
{
   RegionStatistics blackStats;
   blackStats.min = 0.0;
   blackStats.max = 0.0;
   blackStats.mean = 0.0;
   blackStats.median = 0.0;
   blackStats.stdDev = 0.0;
   blackStats.mad = 0.0;
   blackStats.p01 = 0.0;
   blackStats.p05 = 0.0;
   blackStats.p10 = 0.0;
   blackStats.p25 = 0.0;
   blackStats.p75 = 0.0;
   blackStats.p90 = 0.0;
   blackStats.p95 = 0.0;
   blackStats.p99 = 0.0;
   blackStats.snrEstimate = 0.0;
   blackStats.noiseEstimate = 0.0;
   blackStats.pixelCount = 10000;
   blackStats.dynamicRange = 0.0;

   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         REQUIRE( algo != nullptr );

         // AutoConfigure must not crash, throw, or produce NaN/Inf
         REQUIRE_NOTHROW( algo->AutoConfigure( blackStats ) );

         // After configuring with all-zero stats, Apply should still be safe
         double result = algo->Apply( 0.0 );
         REQUIRE( std::isfinite( result ) );
         REQUIRE( result >= 0.0 );
         REQUIRE( result <= 1.0 );

         result = algo->Apply( 0.5 );
         REQUIRE( std::isfinite( result ) );
         REQUIRE( result >= 0.0 );
         REQUIRE( result <= 1.0 );

         result = algo->Apply( 1.0 );
         REQUIRE( std::isfinite( result ) );
         REQUIRE( result >= 0.0 );
         REQUIRE( result <= 1.0 );
      }
   }
}

TEST_CASE( "AutoConfigure: typical astronomical stats do not crash", "[unit][stretch]" )
{
   // Simulate typical linear astronomical image (signal compressed near 0)
   RegionStatistics astroStats;
   astroStats.min = 0.001;
   astroStats.max = 0.15;
   astroStats.mean = 0.01;
   astroStats.median = 0.008;
   astroStats.stdDev = 0.005;
   astroStats.mad = 0.002;
   astroStats.p01 = 0.002;
   astroStats.p05 = 0.003;
   astroStats.p10 = 0.004;
   astroStats.p25 = 0.006;
   astroStats.p75 = 0.012;
   astroStats.p90 = 0.02;
   astroStats.p95 = 0.04;
   astroStats.p99 = 0.10;
   astroStats.snrEstimate = 10.0;
   astroStats.noiseEstimate = 0.003;
   astroStats.pixelCount = 1000000;
   astroStats.dynamicRange = 2.0;

   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         REQUIRE_NOTHROW( algo->AutoConfigure( astroStats ) );

         // After auto-configure, the stretch should produce meaningful results
         double result = algo->Apply( 0.008 );  // Median input
         REQUIRE( std::isfinite( result ) );
         REQUIRE( result >= 0.0 );
         REQUIRE( result <= 1.0 );
      }
   }
}

// ============================================================================
// StatisticalAutoStretch: signalCompression non-negative (fix regression)
// ============================================================================

TEST_CASE( "StatisticalAutoStretch: signalCompression is non-negative", "[unit][stretch]" )
{
   StatisticalAutoStretch sas;

   // Scenario where p90 < background could cause negative signalCompression
   RegionStatistics stats;
   stats.min = 0.0;
   stats.max = 0.01;
   stats.mean = 0.005;
   stats.median = 0.005;
   stats.stdDev = 0.001;
   stats.mad = 0.0005;
   stats.p01 = 0.001;
   stats.p05 = 0.002;
   stats.p10 = 0.003;
   stats.p25 = 0.004;
   stats.p75 = 0.006;
   stats.p90 = 0.007;
   stats.p95 = 0.008;
   stats.p99 = 0.009;
   stats.snrEstimate = 5.0;
   stats.noiseEstimate = 0.001;
   stats.pixelCount = 10000;

   REQUIRE_NOTHROW( sas.AutoConfigure( stats ) );

   // signalCompression should be non-negative (was a bug where it could go negative)
   double compression = sas.SignalCompression();
   REQUIRE( compression >= 0.0 );
   REQUIRE( std::isfinite( compression ) );

   // Apply should still work
   double result = sas.Apply( 0.005 );
   REQUIRE( std::isfinite( result ) );
   REQUIRE( result >= 0.0 );
   REQUIRE( result <= 1.0 );
}

TEST_CASE( "StatisticalAutoStretch: all-zero stats gives non-negative compression", "[unit][stretch]" )
{
   StatisticalAutoStretch sas;

   RegionStatistics zeroStats;
   zeroStats.min = 0.0;
   zeroStats.max = 0.0;
   zeroStats.mean = 0.0;
   zeroStats.median = 0.0;
   zeroStats.stdDev = 0.0;
   zeroStats.mad = 0.0;
   zeroStats.p01 = 0.0;
   zeroStats.p05 = 0.0;
   zeroStats.p10 = 0.0;
   zeroStats.p25 = 0.0;
   zeroStats.p75 = 0.0;
   zeroStats.p90 = 0.0;
   zeroStats.p95 = 0.0;
   zeroStats.p99 = 0.0;
   zeroStats.pixelCount = 1000;

   REQUIRE_NOTHROW( sas.AutoConfigure( zeroStats ) );

   REQUIRE( sas.SignalCompression() >= 0.0 );
   REQUIRE( std::isfinite( sas.SignalCompression() ) );
}

// ============================================================================
// StretchLibrary: factory creates all 12 algorithms
// ============================================================================

TEST_CASE( "StretchLibrary: Create returns non-null for all types", "[unit][stretch]" )
{
   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = StretchLibrary::Instance().Create( type );
         REQUIRE( algo != nullptr );
      }
   }
}

TEST_CASE( "StretchLibrary: Create by ID string works", "[unit][stretch]" )
{
   const std::vector<std::string> ids = {
      "MTF", "Histogram", "GHS", "ArcSinh", "Log", "Lumpton",
      "RNC", "Photometric", "OTS", "SAS", "Veralux", "StatAuto"
   };

   for ( const auto& id : ids )
   {
      DYNAMIC_SECTION( "ID: " << id )
      {
         auto algo = StretchLibrary::Instance().Create( IsoString( id.c_str() ) );
         REQUIRE( algo != nullptr );
      }
   }
}

TEST_CASE( "StretchLibrary: all 12 algorithms are marked implemented", "[unit][stretch]" )
{
   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      REQUIRE( StretchLibrary::Instance().IsImplemented( type ) );
   }
}

// ============================================================================
// Clone: all algorithms support cloning
// ============================================================================

TEST_CASE( "Stretch Clone: all algorithms produce working clones", "[unit][stretch]" )
{
   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         auto clone = algo->Clone();
         REQUIRE( clone != nullptr );

         // Clone should produce the same output
         double orig = algo->Apply( 0.5 );
         double cloned = clone->Apply( 0.5 );
         REQUIRE( orig == Catch::Approx( cloned ) );
      }
   }
}

// ============================================================================
// ResetParameters: all algorithms support parameter reset
// ============================================================================

TEST_CASE( "Stretch ResetParameters: does not crash", "[unit][stretch]" )
{
   for ( AlgorithmType type : AllAlgorithmTypes )
   {
      DYNAMIC_SECTION( "Algorithm: " << AlgoName( type ) )
      {
         auto algo = MakeAlgorithm( type );
         REQUIRE_NOTHROW( algo->ResetParameters() );

         // After reset, Apply should still work
         double result = algo->Apply( 0.5 );
         REQUIRE( std::isfinite( result ) );
         REQUIRE( result >= 0.0 );
         REQUIRE( result <= 1.0 );
      }
   }
}

// ============================================================================
// Direct instantiation of specific algorithms
// ============================================================================

TEST_CASE( "VeraluxStretch: Apply(0.0) == 0.0 (regression)", "[unit][stretch]" )
{
   VeraluxStretch veralux;
   REQUIRE( veralux.Apply( 0.0 ) == Catch::Approx( 0.0 ).margin( 1e-15 ) );
   REQUIRE( veralux.Apply( 1.0 ) == Catch::Approx( 1.0 ).margin( 1e-15 ) );
}

TEST_CASE( "MTFStretch: midtone 0.5 is identity-like", "[unit][stretch]" )
{
   MTFStretch mtf;
   // Default midtone should be 0.5 (identity for MTF)
   // MTF(x, 0.5) = x for all x
   mtf.SetParameter( "midtone", 0.5 );
   REQUIRE( mtf.Apply( 0.0 ) == Catch::Approx( 0.0 ).margin( 1e-10 ) );
   REQUIRE( mtf.Apply( 0.5 ) == Catch::Approx( 0.5 ).margin( 1e-10 ) );
   REQUIRE( mtf.Apply( 1.0 ) == Catch::Approx( 1.0 ).margin( 1e-10 ) );
}

TEST_CASE( "LogStretch: Apply maps correctly", "[unit][stretch]" )
{
   LogStretch log;
   // Log stretch should preserve 0 and 1
   REQUIRE( log.Apply( 0.0 ) == Catch::Approx( 0.0 ).margin( 1e-10 ) );
   REQUIRE( log.Apply( 1.0 ) == Catch::Approx( 1.0 ).margin( 1e-10 ) );
   // Mid values should be boosted (log is concave up from 0)
   double mid = log.Apply( 0.5 );
   REQUIRE( mid > 0.0 );
   REQUIRE( mid <= 1.0 );
}

TEST_CASE( "ArcSinhStretch: preserves boundaries", "[unit][stretch]" )
{
   ArcSinhStretch arcsinh;
   REQUIRE( arcsinh.Apply( 0.0 ) == Catch::Approx( 0.0 ).margin( 1e-10 ) );
   REQUIRE( arcsinh.Apply( 1.0 ) == Catch::Approx( 1.0 ).margin( 1e-10 ) );
}

TEST_CASE( "GHStretch: preserves boundaries", "[unit][stretch]" )
{
   GHStretch ghs;
   REQUIRE( ghs.Apply( 0.0 ) == Catch::Approx( 0.0 ).margin( 1e-10 ) );
   REQUIRE( ghs.Apply( 1.0 ) == Catch::Approx( 1.0 ).margin( 1e-10 ) );
}
