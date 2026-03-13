// tests/unit/test_cuda_remediation.cpp
// CPU-fallback tests for GPU remediation kernels.
// Validates mathematical correctness of trail re-selection, dust correction,
// and vignetting correction without requiring a CUDA GPU at test time.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "engine/PixelSelector.h"
#include "engine/SubCube.h"

#include <vector>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <cmath>

// ============================================================================
// Trail re-selection tests (CPU fallback via PixelSelector::selectBestZ)
// ============================================================================

TEST_CASE( "Trail re-selection excludes contaminated frame", "[remediation][trail]" )
{
   nukex::SubCube cube( 10, 1, 1 );
   float values[] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.1f,
                       0.1f, 0.1f, 0.1f, 0.1f, 0.8f };
   for ( int z = 0; z < 10; ++z )
      cube.setPixel( z, 0, 0, values[z] );

   cube.allocateMasks();
   cube.setMask( 9, 0, 0, 1 ); // mask the trail frame

   nukex::PixelSelector selector;
   std::vector<double> weights( 10, 1.0 );
   auto result = selector.selectBestZ( cube.zColumnPtr(0, 0), 10, weights,
                                        cube.maskColumnPtr(0, 0) );
   REQUIRE( result.selectedValue == Catch::Approx(0.1f).margin(0.01f) );
}

TEST_CASE( "Trail re-selection with multiple contaminated frames", "[remediation][trail]" )
{
   nukex::SubCube cube( 10, 1, 1 );
   // 7 clean frames at ~0.2, 3 trail-contaminated at 0.9
   float values[] = { 0.2f, 0.2f, 0.2f, 0.2f, 0.2f,
                       0.2f, 0.2f, 0.9f, 0.9f, 0.9f };
   for ( int z = 0; z < 10; ++z )
      cube.setPixel( z, 0, 0, values[z] );

   cube.allocateMasks();
   cube.setMask( 7, 0, 0, 1 );
   cube.setMask( 8, 0, 0, 1 );
   cube.setMask( 9, 0, 0, 1 );

   nukex::PixelSelector selector;
   std::vector<double> weights( 10, 1.0 );
   auto result = selector.selectBestZ( cube.zColumnPtr(0, 0), 10, weights,
                                        cube.maskColumnPtr(0, 0) );
   // Should get clean median (~0.2), not trail-contaminated value
   REQUIRE( result.selectedValue == Catch::Approx(0.2f).margin(0.02f) );
}

TEST_CASE( "Trail re-selection falls back when too many masked", "[remediation][trail]" )
{
   // 5 frames, 4 masked -- should fall back to using all frames
   nukex::SubCube cube( 5, 1, 1 );
   float values[] = { 0.1f, 0.1f, 0.5f, 0.5f, 0.5f };
   for ( int z = 0; z < 5; ++z )
      cube.setPixel( z, 0, 0, values[z] );

   cube.allocateMasks();
   cube.setMask( 0, 0, 0, 1 );
   cube.setMask( 1, 0, 0, 1 );
   cube.setMask( 2, 0, 0, 1 );
   cube.setMask( 3, 0, 0, 1 );
   // Only frame 4 is clean -- fewer than 3, so fallback to all

   nukex::PixelSelector selector;
   std::vector<double> weights( 5, 1.0 );
   auto result = selector.selectBestZ( cube.zColumnPtr(0, 0), 5, weights,
                                        cube.maskColumnPtr(0, 0) );
   // Should still produce a valid result (using all frames)
   REQUIRE( result.selectedValue > 0.0f );
   REQUIRE( result.selectedValue < 1.0f );
}

// ============================================================================
// Dust neighbor-ratio correction tests (CPU reference implementation)
// ============================================================================

namespace {

// CPU reference: dust neighbor-ratio correction
// For each dust pixel, compute mean of clean (non-dust, non-zero) neighbors,
// then multiply pixel by clamp(neighborMean / pixelValue, 1.0, maxRatio).
void cpuDustCorrection(
   const float* channelResult,
   int width, int height,
   const uint8_t* dustMask,
   int neighborRadius,
   float maxRatio,
   float* correctedOutput )
{
   std::memcpy( correctedOutput, channelResult,
                width * height * sizeof(float) );

   for ( int y = 0; y < height; ++y ) {
      for ( int x = 0; x < width; ++x ) {
         int idx = y * width + x;
         if ( dustMask[idx] == 0 ) continue; // not a dust pixel

         float pixVal = channelResult[idx];
         if ( pixVal <= 0.0f ) continue; // avoid division by zero

         // Scan neighborhood
         double neighborSum = 0.0;
         int neighborCount = 0;
         for ( int dy = -neighborRadius; dy <= neighborRadius; ++dy ) {
            for ( int dx = -neighborRadius; dx <= neighborRadius; ++dx ) {
               int ny = y + dy;
               int nx = x + dx;
               if ( ny < 0 || ny >= height || nx < 0 || nx >= width ) continue;
               int nIdx = ny * width + nx;
               if ( dustMask[nIdx] != 0 ) continue; // skip other dust pixels
               float nVal = channelResult[nIdx];
               if ( nVal <= 1e-6f ) continue; // skip near-zero
               neighborSum += nVal;
               ++neighborCount;
            }
         }

         if ( neighborCount == 0 ) continue; // no clean neighbors

         float neighborMean = static_cast<float>( neighborSum / neighborCount );
         float ratio = neighborMean / pixVal;
         ratio = std::max( 1.0f, std::min( ratio, maxRatio ) );
         correctedOutput[idx] = pixVal * ratio;
      }
   }
}

// CPU reference: vignetting multiplicative correction
void cpuVignettingCorrection(
   const float* channelResult,
   const float* correctionMap,
   int width, int height,
   float* correctedOutput )
{
   for ( int i = 0; i < width * height; ++i )
      correctedOutput[i] = channelResult[i] * correctionMap[i];
}

} // anonymous namespace

TEST_CASE( "Dust correction brightens attenuated pixels", "[remediation][dust]" )
{
   // 5x5 image, center pixel is a dust mote (attenuated)
   const int W = 5, H = 5;
   std::vector<float> image( W * H, 1.0f ); // uniform background
   image[2 * W + 2] = 0.5f;                 // dust-attenuated center pixel

   std::vector<uint8_t> mask( W * H, 0 );
   mask[2 * W + 2] = 1; // mark center as dust

   std::vector<float> output( W * H );
   cpuDustCorrection( image.data(), W, H, mask.data(),
                       1, 3.0f, output.data() );

   // Center pixel should be corrected toward neighbor mean (~1.0)
   // ratio = 1.0 / 0.5 = 2.0, corrected = 0.5 * 2.0 = 1.0
   REQUIRE( output[2 * W + 2] == Catch::Approx(1.0f).margin(0.01f) );

   // Non-dust pixels should be unchanged
   REQUIRE( output[0] == 1.0f );
   REQUIRE( output[W * H - 1] == 1.0f );
}

TEST_CASE( "Dust correction respects max ratio", "[remediation][dust]" )
{
   const int W = 5, H = 5;
   std::vector<float> image( W * H, 1.0f );
   image[2 * W + 2] = 0.01f; // severely attenuated

   std::vector<uint8_t> mask( W * H, 0 );
   mask[2 * W + 2] = 1;

   std::vector<float> output( W * H );
   float maxRatio = 3.0f;
   cpuDustCorrection( image.data(), W, H, mask.data(),
                       1, maxRatio, output.data() );

   // ratio = 1.0 / 0.01 = 100, clamped to maxRatio=3.0
   // corrected = 0.01 * 3.0 = 0.03
   REQUIRE( output[2 * W + 2] == Catch::Approx(0.03f).margin(0.001f) );
}

TEST_CASE( "Dust correction skips non-dust pixels", "[remediation][dust]" )
{
   const int W = 3, H = 3;
   std::vector<float> image( W * H, 0.5f );
   std::vector<uint8_t> mask( W * H, 0 ); // no dust

   std::vector<float> output( W * H );
   cpuDustCorrection( image.data(), W, H, mask.data(),
                       1, 3.0f, output.data() );

   for ( int i = 0; i < W * H; ++i )
      REQUIRE( output[i] == image[i] );
}

// ============================================================================
// Vignetting multiplicative correction tests
// ============================================================================

TEST_CASE( "Vignetting correction applies multiplicative map", "[remediation][vignetting]" )
{
   const int W = 4, H = 4;
   std::vector<float> image( W * H, 0.5f );
   std::vector<float> corrMap( W * H, 1.0f );

   // Corner pixel needs 2x correction (typical vignetting)
   corrMap[0] = 2.0f;
   corrMap[W - 1] = 1.5f;

   std::vector<float> output( W * H );
   cpuVignettingCorrection( image.data(), corrMap.data(), W, H, output.data() );

   REQUIRE( output[0] == Catch::Approx(1.0f) );     // 0.5 * 2.0
   REQUIRE( output[W - 1] == Catch::Approx(0.75f) ); // 0.5 * 1.5
   REQUIRE( output[W] == Catch::Approx(0.5f) );      // 0.5 * 1.0
}

TEST_CASE( "Vignetting correction identity map is no-op", "[remediation][vignetting]" )
{
   const int W = 3, H = 3;
   std::vector<float> image = { 0.1f, 0.2f, 0.3f,
                                  0.4f, 0.5f, 0.6f,
                                  0.7f, 0.8f, 0.9f };
   std::vector<float> corrMap( W * H, 1.0f ); // identity

   std::vector<float> output( W * H );
   cpuVignettingCorrection( image.data(), corrMap.data(), W, H, output.data() );

   for ( int i = 0; i < W * H; ++i )
      REQUIRE( output[i] == image[i] );
}
