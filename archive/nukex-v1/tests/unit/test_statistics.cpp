// NukeX Unit Tests - Statistics (DistributionFitter + PixelStackAnalyzer)
// Regression tests for fixed bugs in statistical helper functions

#include <catch_amalgamated.hpp>
#include "helpers/TestHelpers.h"

#include "DistributionFitter.h"
#include "PixelStackAnalyzer.h"

#include <vector>
#include <cmath>
#include <numeric>
#include <algorithm>

using namespace pcl;

// ============================================================================
// ComputeMedian
// ============================================================================

TEST_CASE( "ComputeMedian: odd N returns middle value", "[unit][statistics]" )
{
   // DistributionFitter (double)
   {
      std::vector<double> v = { 5.0, 1.0, 3.0 };
      double median = DistributionFitter::ComputeMedian( v );
      REQUIRE( median == Catch::Approx( 3.0 ) );
   }

   // PixelStackAnalyzer (float)
   {
      std::vector<float> v = { 5.0f, 1.0f, 3.0f };
      float median = PixelStackAnalyzer::ComputeMedian( v );
      REQUIRE( median == Catch::Approx( 3.0f ) );
   }
}

TEST_CASE( "ComputeMedian: even N returns average of two middle values", "[unit][statistics]" )
{
   // DistributionFitter (double)
   {
      std::vector<double> v = { 4.0, 1.0, 3.0, 2.0 };
      double median = DistributionFitter::ComputeMedian( v );
      REQUIRE( median == Catch::Approx( 2.5 ) );
   }

   // PixelStackAnalyzer (float)
   {
      std::vector<float> v = { 4.0f, 1.0f, 3.0f, 2.0f };
      float median = PixelStackAnalyzer::ComputeMedian( v );
      REQUIRE( median == Catch::Approx( 2.5f ) );
   }
}

TEST_CASE( "ComputeMedian: single element", "[unit][statistics]" )
{
   std::vector<double> v = { 7.0 };
   REQUIRE( DistributionFitter::ComputeMedian( v ) == Catch::Approx( 7.0 ) );

   std::vector<float> vf = { 7.0f };
   REQUIRE( PixelStackAnalyzer::ComputeMedian( vf ) == Catch::Approx( 7.0f ) );
}

TEST_CASE( "ComputeMedian: empty returns 0", "[unit][statistics]" )
{
   std::vector<double> v;
   REQUIRE( DistributionFitter::ComputeMedian( v ) == 0.0 );

   std::vector<float> vf;
   REQUIRE( PixelStackAnalyzer::ComputeMedian( vf ) == 0.0f );
}

TEST_CASE( "ComputeMedian: even N with 2 elements", "[unit][statistics]" )
{
   std::vector<double> v = { 10.0, 20.0 };
   REQUIRE( DistributionFitter::ComputeMedian( v ) == Catch::Approx( 15.0 ) );
}

// ============================================================================
// ComputeMAD - must NOT destroy its input (pass-by-value regression test)
// ============================================================================

TEST_CASE( "ComputeMAD: does not modify caller's vector", "[unit][statistics]" )
{
   // DistributionFitter version takes by value
   {
      std::vector<double> original = { 1.0, 2.0, 3.0, 4.0, 5.0 };
      std::vector<double> copy = original;
      double median = 3.0;
      DistributionFitter::ComputeMAD( copy, median );
      // If the bug existed (pass-by-reference), copy would be destroyed
      REQUIRE( copy == original );
   }

   // PixelStackAnalyzer version takes by value
   {
      std::vector<float> original = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
      std::vector<float> copy = original;
      float median = 3.0f;
      PixelStackAnalyzer::ComputeMAD( copy, median );
      REQUIRE( copy == original );
   }
}

TEST_CASE( "ComputeMAD: correct value for symmetric data", "[unit][statistics]" )
{
   // Data: {1, 2, 3, 4, 5}, median=3
   // Absolute deviations from median: {2, 1, 0, 1, 2}
   // Sorted: {0, 1, 1, 2, 2}, median of deviations = 1.0
   std::vector<double> v = { 1.0, 2.0, 3.0, 4.0, 5.0 };
   double mad = DistributionFitter::ComputeMAD( v, 3.0 );
   REQUIRE( mad == Catch::Approx( 1.0 ) );
}

TEST_CASE( "ComputeMAD: constant data returns 0", "[unit][statistics]" )
{
   std::vector<float> v = { 0.5f, 0.5f, 0.5f, 0.5f };
   float mad = PixelStackAnalyzer::ComputeMAD( v, 0.5f );
   REQUIRE( mad == 0.0f );
}

// ============================================================================
// ComputeStdDev - sample stddev (n-1 denominator), returns 0 for N<2
// ============================================================================

TEST_CASE( "ComputeStdDev: uses sample formula (n-1 denominator)", "[unit][statistics]" )
{
   // Data: {2, 4, 4, 4, 5, 5, 7, 9}, mean = 5.0
   // sum of sq deviations = 9+1+1+1+0+0+4+16 = 32
   // sample variance = 32/7 = 4.571..., sample stddev = sqrt(32/7) ~ 2.138
   std::vector<double> v = { 2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0 };
   double mean = DistributionFitter::ComputeMean( v );
   double sd = DistributionFitter::ComputeStdDev( v, mean );
   double expected = std::sqrt( 32.0 / 7.0 );
   REQUIRE( sd == Catch::Approx( expected ).epsilon( 1e-10 ) );
}

TEST_CASE( "ComputeStdDev: returns 0 for N<2", "[unit][statistics]" )
{
   // Empty
   {
      std::vector<double> v;
      REQUIRE( DistributionFitter::ComputeStdDev( v, 0.0 ) == 0.0 );
   }
   // Single element
   {
      std::vector<double> v = { 5.0 };
      REQUIRE( DistributionFitter::ComputeStdDev( v, 5.0 ) == 0.0 );
   }
   // PixelStackAnalyzer float version
   {
      std::vector<float> v = { 5.0f };
      REQUIRE( PixelStackAnalyzer::ComputeStdDev( v, 5.0f ) == 0.0f );
   }
}

TEST_CASE( "ComputeStdDev: PixelStackAnalyzer uses n-1 denominator", "[unit][statistics]" )
{
   std::vector<float> v = { 2.0f, 4.0f, 4.0f, 4.0f, 5.0f, 5.0f, 7.0f, 9.0f };
   float mean = PixelStackAnalyzer::ComputeMean( v );
   float sd = PixelStackAnalyzer::ComputeStdDev( v, mean );
   float expected = static_cast<float>( std::sqrt( 32.0 / 7.0 ) );
   REQUIRE( sd == Catch::Approx( expected ).epsilon( 1e-4 ) );
}

// ============================================================================
// ComputeMean - accurate for large N (fixed float->double accumulation)
// ============================================================================

TEST_CASE( "ComputeMean: accurate for large N with double accumulation", "[unit][statistics]" )
{
   // Regression: old code used float accumulation, losing precision for large N
   // Build a large vector where float accumulation would lose precision
   const int N = 100000;
   std::vector<float> v( N );
   for ( int i = 0; i < N; ++i )
      v[i] = 1.0f + static_cast<float>( i ) / N;  // values in [1.0, 2.0)

   float mean = PixelStackAnalyzer::ComputeMean( v );
   // Expected mean ~ 1.5 (midpoint of uniformly spaced values)
   REQUIRE( mean == Catch::Approx( 1.5f ).epsilon( 0.001f ) );
}

TEST_CASE( "ComputeMean: DistributionFitter uses double accumulation", "[unit][statistics]" )
{
   const int N = 100000;
   std::vector<double> v( N );
   for ( int i = 0; i < N; ++i )
      v[i] = 1.0 + static_cast<double>( i ) / N;

   double mean = DistributionFitter::ComputeMean( v );
   REQUIRE( mean == Catch::Approx( 1.5 ).epsilon( 1e-4 ) );
}

TEST_CASE( "ComputeMean: empty returns 0", "[unit][statistics]" )
{
   std::vector<double> v;
   REQUIRE( DistributionFitter::ComputeMean( v ) == 0.0 );

   std::vector<float> vf;
   REQUIRE( PixelStackAnalyzer::ComputeMean( vf ) == 0.0f );
}

// ============================================================================
// ComputeSkewness - consistent between DistributionFitter and PixelStackAnalyzer
// ============================================================================

TEST_CASE( "ComputeSkewness: consistent between DF and PSA", "[unit][statistics]" )
{
   // Use symmetric data -> skewness should be near 0
   std::vector<double> dv = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0 };
   std::vector<float> fv( dv.begin(), dv.end() );

   double dMean = DistributionFitter::ComputeMean( dv );
   double dSigma = DistributionFitter::ComputeStdDev( dv, dMean );
   double dSkew = DistributionFitter::ComputeSkewness( dv, dMean, dSigma );

   float fMean = PixelStackAnalyzer::ComputeMean( fv );
   float fSigma = PixelStackAnalyzer::ComputeStdDev( fv, fMean );
   float fSkew = PixelStackAnalyzer::ComputeSkewness( fv, fMean, fSigma );

   // Both should use the same adjusted formula: sum3 * n / ((n-1) * (n-2))
   // Use margin for near-zero comparison (epsilon is relative, useless near 0)
   REQUIRE( dSkew == Catch::Approx( static_cast<double>( fSkew ) ).margin( 0.01 ) );
   // Symmetric data should have near-zero skewness
   REQUIRE( std::abs( dSkew ) < 0.01 );
}

TEST_CASE( "ComputeSkewness: positive skew for right-skewed data", "[unit][statistics]" )
{
   std::vector<double> v = { 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 10.0, 20.0, 50.0 };
   double mean = DistributionFitter::ComputeMean( v );
   double sigma = DistributionFitter::ComputeStdDev( v, mean );
   double skew = DistributionFitter::ComputeSkewness( v, mean, sigma );
   REQUIRE( skew > 0.0 );
}

TEST_CASE( "ComputeSkewness: returns 0 for N<3 or zero sigma", "[unit][statistics]" )
{
   std::vector<double> v2 = { 1.0, 2.0 };
   REQUIRE( DistributionFitter::ComputeSkewness( v2, 1.5, 0.5 ) == 0.0 );

   std::vector<float> vf = { 1.0f, 2.0f, 3.0f };
   REQUIRE( PixelStackAnalyzer::ComputeSkewness( vf, 2.0f, 0.0f ) == 0.0f );
}

// ============================================================================
// ComputeKurtosis - consistent between DF and PSA (fixed naive formula)
// ============================================================================

TEST_CASE( "ComputeKurtosis: consistent between DF and PSA", "[unit][statistics]" )
{
   // Both now use the same naive estimator: (sum4/n) - 3.0
   std::vector<double> dv = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0 };
   std::vector<float> fv( dv.begin(), dv.end() );

   double dMean = DistributionFitter::ComputeMean( dv );
   double dSigma = DistributionFitter::ComputeStdDev( dv, dMean );
   double dKurt = DistributionFitter::ComputeKurtosis( dv, dMean, dSigma );

   float fMean = PixelStackAnalyzer::ComputeMean( fv );
   float fSigma = PixelStackAnalyzer::ComputeStdDev( fv, fMean );
   float fKurt = PixelStackAnalyzer::ComputeKurtosis( fv, fMean, fSigma );

   REQUIRE( dKurt == Catch::Approx( static_cast<double>( fKurt ) ).epsilon( 0.05 ) );
}

TEST_CASE( "ComputeKurtosis: Gaussian-like data has excess kurtosis near 0", "[unit][statistics]" )
{
   // Generate pseudo-Gaussian via central limit theorem: sum of 12 uniform [0,1)
   // This gives approximately N(6, 1). Use a fixed seed for reproducibility.
   std::mt19937 rng( 42 );
   std::uniform_real_distribution<double> dist( 0.0, 1.0 );

   const int N = 10000;
   std::vector<double> v( N );
   for ( int i = 0; i < N; ++i )
   {
      double sum = 0;
      for ( int j = 0; j < 12; ++j )
         sum += dist( rng );
      v[i] = sum;  // approximately N(6, 1)
   }

   double mean = DistributionFitter::ComputeMean( v );
   double sigma = DistributionFitter::ComputeStdDev( v, mean );
   double kurt = DistributionFitter::ComputeKurtosis( v, mean, sigma );

   // Excess kurtosis of Gaussian = 0, allow generous tolerance
   REQUIRE( std::abs( kurt ) < 0.3 );
}

TEST_CASE( "ComputeKurtosis: returns 0 for N<4 or zero sigma", "[unit][statistics]" )
{
   std::vector<double> v3 = { 1.0, 2.0, 3.0 };
   REQUIRE( DistributionFitter::ComputeKurtosis( v3, 2.0, 1.0 ) == 0.0 );

   std::vector<float> vf = { 1.0f, 2.0f, 3.0f, 4.0f };
   REQUIRE( PixelStackAnalyzer::ComputeKurtosis( vf, 2.5f, 0.0f ) == 0.0f );
}

// ============================================================================
// FitDistribution - smoke tests
// ============================================================================

TEST_CASE( "FitDistribution: Gaussian-like data returns Gaussian type", "[unit][statistics]" )
{
   // Tight, symmetric data
   std::vector<double> v = { 0.50, 0.51, 0.49, 0.50, 0.52, 0.48, 0.51, 0.49, 0.50, 0.50 };
   DistributionFitter fitter;
   DistributionParams params = fitter.FitDistribution( v );

   REQUIRE( params.type == DistributionType::Gaussian );
   REQUIRE( params.mu == Catch::Approx( 0.50 ).epsilon( 0.02 ) );
   REQUIRE( params.sigma > 0 );
   REQUIRE( params.quality > 0 );
}

TEST_CASE( "FitDistribution: PSA version returns valid distribution", "[unit][statistics]" )
{
   std::vector<float> v = { 0.50f, 0.51f, 0.49f, 0.50f, 0.52f, 0.48f, 0.51f, 0.49f };
   PixelStackAnalyzer psa;
   StackDistributionParams params = psa.FitDistribution( v );

   REQUIRE( params.mu == Catch::Approx( 0.50f ).epsilon( 0.02f ) );
   REQUIRE( params.sigma > 0 );
   REQUIRE( params.quality > 0 );
}

TEST_CASE( "FitDistribution: empty input returns defaults", "[unit][statistics]" )
{
   DistributionFitter fitter;
   DistributionParams params = fitter.FitDistribution( {} );
   REQUIRE( params.mu == 0.0 );
   REQUIRE( params.sigma == 0.0 );
}

TEST_CASE( "FitDistribution: skewed data detected as skewed or lognormal", "[unit][statistics]" )
{
   // Heavily right-skewed data
   std::vector<double> v;
   for ( int i = 0; i < 100; ++i )
      v.push_back( 0.1 + 0.01 * (i % 10) );
   // Add some extreme high values
   v.push_back( 5.0 );
   v.push_back( 10.0 );
   v.push_back( 20.0 );

   DistributionFitter fitter;
   DistributionParams params = fitter.FitDistribution( v );
   // Should not be classified as Gaussian
   REQUIRE( (params.type == DistributionType::Skewed || params.type == DistributionType::Lognormal) );
}

// ============================================================================
// AnalyzePixel - sigma clipping with known outliers
// ============================================================================

TEST_CASE( "AnalyzePixel: rejects known outlier at 25 sigma", "[unit][statistics]" )
{
   // Simulate M42 Trapezium scenario from CLAUDE.md
   std::vector<float> values = { 0.82f, 0.79f, 0.31f, 0.84f, 0.80f };
   // Frame 2 (0.31) is the outlier - cloud contamination

   PixelStackAnalyzer psa;
   PixelStackMetadata meta = psa.AnalyzePixel( values );

   // Frame 2 (index 2) should be marked as outlier
   REQUIRE( meta.IsOutlier( 2 ) );
   REQUIRE( meta.OutlierCount() >= 1 );

   // Selected value should NOT be the outlier
   REQUIRE( meta.selectedValue != Catch::Approx( 0.31f ).epsilon( 0.01f ) );
}

TEST_CASE( "AnalyzePixel: no outliers in clean data", "[unit][statistics]" )
{
   std::vector<float> values = { 0.50f, 0.51f, 0.49f, 0.50f, 0.52f, 0.48f, 0.51f, 0.49f };

   PixelStackAnalyzer psa;
   PixelStackMetadata meta = psa.AnalyzePixel( values );

   REQUIRE( meta.OutlierCount() == 0 );
   REQUIRE( meta.confidence > 0.0f );
}

TEST_CASE( "AnalyzePixel: respects minFramesForStats", "[unit][statistics]" )
{
   // Default minFramesForStats is 3, so with 2 frames we should get low confidence
   std::vector<float> values = { 0.50f, 0.80f };

   PixelStackAnalyzer psa;
   PixelStackMetadata meta = psa.AnalyzePixel( values );

   REQUIRE( meta.confidence == 0.0f );
   // Should still produce a selected value (median)
   REQUIRE( meta.selectedValue == Catch::Approx( 0.65f ).epsilon( 0.01f ) );
}

// ============================================================================
// Sigma floor - contaminated stack (30% outliers) still rejects them
// ============================================================================

TEST_CASE( "Sigma floor: 30% contaminated stack rejects outliers", "[unit][statistics]" )
{
   // 10 frames: 7 good at ~0.5, 3 bad outliers at 0.95
   // Without MAD-based sigma floor, iterative clipping could death-spiral:
   //   sigma shrinks each pass, causing cascading false rejections
   std::vector<float> values = {
      0.50f, 0.51f, 0.49f, 0.52f, 0.48f, 0.51f, 0.49f,  // 7 good
      0.95f, 0.93f, 0.97f                                   // 3 outliers
   };

   PixelStackAnalyzer psa;
   PixelStackMetadata meta = psa.AnalyzePixel( values );

   // The 3 outliers (indices 7, 8, 9) should be rejected
   int outlierCount = meta.OutlierCount();
   REQUIRE( outlierCount >= 2 );  // At least most outliers caught

   // But we must NOT reject the good frames
   // Selected value should be near the good cluster ~0.50
   REQUIRE( meta.selectedValue == Catch::Approx( 0.50f ).epsilon( 0.05f ) );

   // Verify total frames
   REQUIRE( meta.totalFrames == 10 );
}

TEST_CASE( "Sigma floor: constant data does not crash or over-reject", "[unit][statistics]" )
{
   // All identical values: sigma = 0, MAD = 0
   // Without floor, division by zero could occur
   std::vector<float> values( 10, 0.5f );

   PixelStackAnalyzer psa;
   PixelStackMetadata meta = psa.AnalyzePixel( values );

   REQUIRE( meta.OutlierCount() == 0 );
   REQUIRE( meta.selectedValue == Catch::Approx( 0.5f ) );
}

TEST_CASE( "Sigma floor: extreme outlier in tight cluster is rejected", "[unit][statistics]" )
{
   // Use a very tight cluster with a massive outlier to ensure detection
   // 10 frames at ~0.50 +/- 0.001, one outlier at 0.99
   std::vector<float> values = {
      0.500f, 0.501f, 0.499f, 0.500f, 0.502f,
      0.99f,  // index 5 = extreme outlier
      0.498f, 0.501f, 0.500f, 0.499f
   };

   PixelStackAnalyzer psa;
   PixelStackMetadata meta = psa.AnalyzePixel( values );

   // The outlier at index 5 should be detected via iterative sigma clipping
   // since the MAD-based sigma floor preserves the tight cluster's scale
   REQUIRE( meta.OutlierCount() >= 1 );
   REQUIRE( meta.selectedValue == Catch::Approx( 0.5f ).epsilon( 0.01f ) );
}
