// ----------------------------------------------------------------------------
// test_flat_calibrator.cpp — Unit tests for FlatCalibrator
// ----------------------------------------------------------------------------

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/FlatCalibrator.h"

#include <vector>
#include <cmath>

using Catch::Approx;

// ============================================================================
// Median-stack + normalize — verify master flat values
// ============================================================================

TEST_CASE( "FlatCalibrator median-stacks and normalizes", "[flat]" )
{
   // 3 flat frames: uniform 0.8 brightness with a circular deficit at center
   const int W = 100, H = 100;
   const int cx = 50, cy = 50, moteR = 10;

   nukex::FlatCalibrator cal;
   for ( int f = 0; f < 3; ++f )
   {
      std::vector<float> r( W * H ), g( W * H ), b( W * H );
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            float val = 0.8f;
            double d = std::sqrt( double( (x - cx) * (x - cx) + (y - cy) * (y - cy) ) );
            if ( d < moteR )
               val = 0.6f;  // 25% deficit
            // Add slight per-frame variation
            val += 0.01f * ( f - 1 );
            r[y * W + x] = g[y * W + x] = b[y * W + x] = val;
         }
      cal.addFrame( r.data(), g.data(), b.data(), W, H );
   }

   cal.buildMasterFlat();
   REQUIRE( cal.isReady() );
   REQUIRE( cal.frameCount() == 3 );

   // Master flat should be normalized: clean pixels ~ 1.0, mote < 1.0
   // The median of {0.79, 0.80, 0.81} = 0.80 is the normalization factor,
   // so clean = 0.80 / 0.80 = 1.0
   // Mote pixels: median(0.59, 0.60, 0.61) = 0.60, normalized = 0.60 / 0.80 = 0.75
}

// ============================================================================
// Calibrate — verify mote removal from a light frame
// ============================================================================

TEST_CASE( "FlatCalibrator removes dust mote from light frame", "[flat]" )
{
   // Build a flat with a known mote, calibrate a light that has the same mote
   const int W = 100, H = 100;
   const int cx = 50, cy = 50, moteR = 10;
   const float flatBg = 0.8f, flatMote = 0.6f;

   nukex::FlatCalibrator cal;
   // Use 5 identical flats (median = each frame)
   for ( int f = 0; f < 5; ++f )
   {
      std::vector<float> ch( W * H );
      for ( int y = 0; y < H; ++y )
         for ( int x = 0; x < W; ++x )
         {
            double d = std::sqrt( double( (x - cx) * (x - cx) + (y - cy) * (y - cy) ) );
            ch[y * W + x] = ( d < moteR ) ? flatMote : flatBg;
         }
      cal.addFrame( ch.data(), ch.data(), ch.data(), W, H );
   }
   cal.buildMasterFlat();

   // Create a "light" frame with the same mote pattern applied to a sky signal
   float skyBg = 0.1f;
   std::vector<float> lightR( W * H ), lightG( W * H ), lightB( W * H );
   for ( int y = 0; y < H; ++y )
      for ( int x = 0; x < W; ++x )
      {
         double d = std::sqrt( double( (x - cx) * (x - cx) + (y - cy) * (y - cy) ) );
         float moteAtten = ( d < moteR ) ? ( flatMote / flatBg ) : 1.0f;  // 0.75
         lightR[y * W + x] = lightG[y * W + x] = lightB[y * W + x] = skyBg * moteAtten;
      }

   cal.calibrate( lightR.data(), lightG.data(), lightB.data(), W, H );

   // After calibration, mote pixels should match background
   float centerVal = lightR[cy * W + cx];
   REQUIRE( centerVal == Approx( skyBg ).margin( skyBg * 0.01f ) );

   // Edge pixel (outside mote) should be unchanged
   float edgeVal = lightR[0];
   REQUIRE( edgeVal == Approx( skyBg ).margin( skyBg * 0.01f ) );
}

// ============================================================================
// Empty frame list — should not crash
// ============================================================================

TEST_CASE( "FlatCalibrator handles empty frame list", "[flat]" )
{
   nukex::FlatCalibrator cal;
   REQUIRE_FALSE( cal.isReady() );
   REQUIRE( cal.frameCount() == 0 );
   // buildMasterFlat with no frames should not crash
   cal.buildMasterFlat();
   REQUIRE_FALSE( cal.isReady() );
}

// ============================================================================
// Single frame — median of 1 value is itself
// ============================================================================

TEST_CASE( "FlatCalibrator handles single frame", "[flat]" )
{
   const int W = 50, H = 50;
   std::vector<float> ch( W * H, 0.7f );
   nukex::FlatCalibrator cal;
   cal.addFrame( ch.data(), ch.data(), ch.data(), W, H );
   cal.buildMasterFlat();
   REQUIRE( cal.isReady() );
   // Single frame median = itself, normalized = 1.0 everywhere
}
