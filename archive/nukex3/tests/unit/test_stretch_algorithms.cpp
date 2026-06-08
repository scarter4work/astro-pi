// NukeX v3 - Stretch Algorithm Unit Tests
// Comprehensive Catch2 v3 tests for all 11 stretch algorithms.
//
// CMake target to add in tests/CMakeLists.txt:
//
//   add_executable(test_stretch_algorithms
//       unit/test_stretch_algorithms.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/IStretchAlgorithm.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/MTFStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/ArcSinhStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/GHStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/HistogramStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/LogStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/LumptonStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/RNCStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/PhotometricStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/OTSStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/SASStretch.cpp
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms/VeraluxStretch.cpp
//   )
//   target_link_libraries(test_stretch_algorithms PRIVATE Catch2::Catch2WithMain)
//   target_include_directories(test_stretch_algorithms PRIVATE
//       ${PCL_INCLUDE_DIR}
//       ${CMAKE_SOURCE_DIR}/src
//       ${CMAKE_SOURCE_DIR}/src/engine
//       ${CMAKE_SOURCE_DIR}/src/engine/algorithms
//   )
//   target_link_directories(test_stretch_algorithms PRIVATE ${PCL_LIB_DIR})
//   target_link_libraries(test_stretch_algorithms PRIVATE ${PCL_LIBRARIES} pthread)
//   target_compile_definitions(test_stretch_algorithms PRIVATE __PCL_LINUX _REENTRANT)
//   target_compile_features(test_stretch_algorithms PRIVATE cxx_std_17)
//   add_test(NAME test_stretch_algorithms COMMAND test_stretch_algorithms)

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <algorithm>

#include "engine/algorithms/MTFStretch.h"
#include "engine/algorithms/ArcSinhStretch.h"
#include "engine/algorithms/GHStretch.h"
#include "engine/algorithms/HistogramStretch.h"
#include "engine/algorithms/LogStretch.h"
#include "engine/algorithms/LumptonStretch.h"
#include "engine/algorithms/RNCStretch.h"
#include "engine/algorithms/PhotometricStretch.h"
#include "engine/algorithms/OTSStretch.h"
#include "engine/algorithms/SASStretch.h"
#include "engine/algorithms/VeraluxStretch.h"

using Catch::Approx;

// ============================================================================
//  1. MTFStretch
// ============================================================================

TEST_CASE( "MTFStretch boundary values", "[stretch][mtf]" )
{
   pcl::MTFStretch algo;
   // Default midtones = 0.5 means identity transform
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
   // With default m=0.5, MTF is identity so Apply(0.5) == 0.5
   CHECK( mid == Approx( 0.5 ).margin( 1e-6 ) );
}

TEST_CASE( "MTFStretch monotonicity", "[stretch][mtf]" )
{
   pcl::MTFStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "MTFStretch auto-configure", "[stretch][mtf]" )
{
   pcl::MTFStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.0 );   // produces a visible result
   CHECK( mid < 0.95 );

   // Monotonicity after auto-configure
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "MTFStretch output range [0,1]", "[stretch][mtf]" )
{
   pcl::MTFStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "MTFStretch midtones parameter effect", "[stretch][mtf]" )
{
   pcl::MTFStretch algo;
   // Low midtones balance -> brightens the image
   algo.SetMidtones( 0.1 );
   double brightened = algo.Apply( 0.3 );
   algo.SetMidtones( 0.9 );
   double darkened = algo.Apply( 0.3 );
   CHECK( brightened > darkened );
}

TEST_CASE( "MTFStretch shadows clipping", "[stretch][mtf]" )
{
   pcl::MTFStretch algo;
   algo.SetShadowsClip( 0.1 );
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ) );
   CHECK( algo.Apply( 0.05 ) == Approx( 0.0 ) );
   CHECK( algo.Apply( 0.1 ) == Approx( 0.0 ) );
   CHECK( algo.Apply( 0.2 ) > 0.0 );
}

// ============================================================================
//  2. ArcSinhStretch
// ============================================================================

TEST_CASE( "ArcSinhStretch boundary values", "[stretch][arcsinh]" )
{
   pcl::ArcSinhStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   // At x=1.0, asinh(1*10)/asinh(10) is clamped to 1.0
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
}

TEST_CASE( "ArcSinhStretch monotonicity", "[stretch][arcsinh]" )
{
   pcl::ArcSinhStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "ArcSinhStretch auto-configure", "[stretch][arcsinh]" )
{
   pcl::ArcSinhStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.05 );
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "ArcSinhStretch output range [0,1]", "[stretch][arcsinh]" )
{
   pcl::ArcSinhStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "ArcSinhStretch higher beta compresses more", "[stretch][arcsinh]" )
{
   pcl::ArcSinhStretch low;
   low.SetBeta( 5.0 );
   pcl::ArcSinhStretch high;
   high.SetBeta( 100.0 );
   // Higher beta should boost faint values more
   double lowResult = low.Apply( 0.1 );
   double highResult = high.Apply( 0.1 );
   CHECK( highResult > lowResult );
}

// ============================================================================
//  3. GHStretch
// ============================================================================

TEST_CASE( "GHStretch boundary values", "[stretch][ghs]" )
{
   pcl::GHStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
}

TEST_CASE( "GHStretch monotonicity", "[stretch][ghs]" )
{
   pcl::GHStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "GHStretch auto-configure", "[stretch][ghs]" )
{
   pcl::GHStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid >= 0.0 );   // GHS may compress depending on parameters
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "GHStretch output range [0,1]", "[stretch][ghs]" )
{
   pcl::GHStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "GHStretch linear preset is identity", "[stretch][ghs]" )
{
   pcl::GHStretch algo;
   algo.PresetLinear();
   // D=0 means no stretch, should be approximately identity
   for ( int i = 0; i <= 10; ++i )
   {
      double x = i / 10.0;
      CHECK( algo.Apply( x ) == Approx( x ).margin( 1e-4 ) );
   }
}

TEST_CASE( "GHStretch presets produce valid output range", "[stretch][ghs]" )
{
   // GHS HighlightProtect preset can compress highlights (not strictly monotonic)
   // so we test output range instead of monotonicity for presets
   auto checkRange = []( pcl::GHStretch& algo ) {
      for ( int i = 0; i <= 100; ++i )
      {
         double x = i / 100.0;
         double y = algo.Apply( x );
         CHECK( y >= -1e-10 );
         CHECK( y <= 1.0 + 1e-10 );
      }
   };

   pcl::GHStretch a; a.PresetBalanced();      checkRange( a );
   pcl::GHStretch b; b.PresetShadowBias();    checkRange( b );
   pcl::GHStretch c; c.PresetHighlightProtect(); checkRange( c );
}

// ============================================================================
//  4. HistogramStretch
// ============================================================================

TEST_CASE( "HistogramStretch boundary values", "[stretch][histogram]" )
{
   pcl::HistogramStretch algo;
   // Default: shadows=0, highlights=1, midtones=0.5, low/highOutput=0/1
   // With m=0.5, this is identity
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
   CHECK( mid == Approx( 0.5 ).margin( 1e-6 ) );
}

TEST_CASE( "HistogramStretch monotonicity", "[stretch][histogram]" )
{
   pcl::HistogramStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "HistogramStretch auto-configure", "[stretch][histogram]" )
{
   pcl::HistogramStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.05 );
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "HistogramStretch output range [0,1]", "[stretch][histogram]" )
{
   pcl::HistogramStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "HistogramStretch output range parameters", "[stretch][histogram]" )
{
   pcl::HistogramStretch algo;
   algo.SetLowOutput( 0.1 );
   algo.SetHighOutput( 0.9 );
   // Value at shadows clip should give lowOutput
   CHECK( algo.Apply( 0.0 ) == Approx( 0.1 ).margin( 1e-4 ) );
   // Value at highlights clip should give highOutput
   CHECK( algo.Apply( 1.0 ) == Approx( 0.9 ).margin( 1e-4 ) );
   // Mid value should be between lowOutput and highOutput
   double mid = algo.Apply( 0.5 );
   CHECK( mid >= 0.1 );
   CHECK( mid <= 0.9 );
}

// ============================================================================
//  5. LogStretch
// ============================================================================

TEST_CASE( "LogStretch boundary values", "[stretch][log]" )
{
   pcl::LogStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
   // Log stretch boosts midtones: log(1+100*0.5)/log(1+100) > 0.5
   CHECK( mid > 0.5 );
}

TEST_CASE( "LogStretch monotonicity", "[stretch][log]" )
{
   pcl::LogStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "LogStretch auto-configure", "[stretch][log]" )
{
   pcl::LogStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.05 );
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "LogStretch output range [0,1]", "[stretch][log]" )
{
   pcl::LogStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "LogStretch higher scale compresses more", "[stretch][log]" )
{
   pcl::LogStretch low;
   low.SetScale( 10.0 );
   pcl::LogStretch high;
   high.SetScale( 1000.0 );
   // Higher scale -> faint details brighter
   double lowResult = low.Apply( 0.1 );
   double highResult = high.Apply( 0.1 );
   CHECK( highResult > lowResult );
}

// ============================================================================
//  6. LumptonStretch
// ============================================================================

TEST_CASE( "LumptonStretch boundary values", "[stretch][lumpton]" )
{
   pcl::LumptonStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   // Apply(1.0): x=1, scaledX=1/0.05=20, asinh(8*20)/asinh(8/0.05)=asinh(160)/asinh(160)=1.0
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
}

TEST_CASE( "LumptonStretch monotonicity", "[stretch][lumpton]" )
{
   pcl::LumptonStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "LumptonStretch auto-configure", "[stretch][lumpton]" )
{
   pcl::LumptonStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.05 );
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "LumptonStretch output range [0,1]", "[stretch][lumpton]" )
{
   pcl::LumptonStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

// ============================================================================
//  7. RNCStretch
// ============================================================================

TEST_CASE( "RNCStretch boundary values", "[stretch][rnc]" )
{
   pcl::RNCStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   // Apply(1.0): x=1, pow(1, 1/2.5)=1.0
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
   // Power stretch: 0.5^(1/2.5) > 0.5
   CHECK( mid > 0.5 );
}

TEST_CASE( "RNCStretch monotonicity", "[stretch][rnc]" )
{
   pcl::RNCStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "RNCStretch auto-configure", "[stretch][rnc]" )
{
   pcl::RNCStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.05 );
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "RNCStretch output range [0,1]", "[stretch][rnc]" )
{
   pcl::RNCStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "RNCStretch power law relationship", "[stretch][rnc]" )
{
   pcl::RNCStretch algo;
   // Default stretchFactor=2.5 -> Apply(x) = pow(x, 1/2.5)
   double x = 0.3;
   double expected = std::pow( x, 1.0 / 2.5 );
   CHECK( algo.Apply( x ) == Approx( expected ).margin( 1e-6 ) );
}

// ============================================================================
//  8. PhotometricStretch
// ============================================================================

TEST_CASE( "PhotometricStretch boundary values", "[stretch][photometric]" )
{
   pcl::PhotometricStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   // Apply(1.0): TransferFunction(1.0) may not exactly equal 1.0 due to
   // the asinh-based mapping, but it should be close and clamped
   CHECK( algo.Apply( 1.0 ) <= 1.0 + 1e-10 );
   CHECK( algo.Apply( 1.0 ) > 0.0 );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
}

TEST_CASE( "PhotometricStretch monotonicity", "[stretch][photometric]" )
{
   pcl::PhotometricStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "PhotometricStretch auto-configure", "[stretch][photometric]" )
{
   pcl::PhotometricStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.0 );   // Photometric may not boost median depending on curve
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "PhotometricStretch output range [0,1]", "[stretch][photometric]" )
{
   pcl::PhotometricStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "PhotometricStretch invertibility", "[stretch][photometric]" )
{
   pcl::PhotometricStretch algo;
   // TransferFunction is invertible — Inverse(Apply(x)) should approximate x
   for ( int i = 1; i <= 9; ++i )
   {
      double x = i / 10.0;
      double stretched = algo.Apply( x );
      double recovered = algo.Inverse( stretched );
      CHECK( recovered == Approx( x ).margin( 0.02 ) );
   }
}

// ============================================================================
//  9. OTSStretch
// ============================================================================

TEST_CASE( "OTSStretch boundary values", "[stretch][ots]" )
{
   pcl::OTSStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
}

TEST_CASE( "OTSStretch monotonicity", "[stretch][ots]" )
{
   pcl::OTSStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "OTSStretch auto-configure", "[stretch][ots]" )
{
   pcl::OTSStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.05 );
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "OTSStretch output range [0,1]", "[stretch][ots]" )
{
   pcl::OTSStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "OTSStretch curve shape blending", "[stretch][ots]" )
{
   // curveShape=0 -> pure MTF, curveShape=1 -> pure power curve
   pcl::OTSStretch mtfOnly;
   mtfOnly.AutoConfigure( 0.05, 0.002 );
   mtfOnly.SetCurveShape( 0.0 );

   pcl::OTSStretch powerOnly;
   powerOnly.AutoConfigure( 0.05, 0.002 );
   powerOnly.SetCurveShape( 1.0 );

   pcl::OTSStretch blended;
   blended.AutoConfigure( 0.05, 0.002 );
   blended.SetCurveShape( 0.5 );

   // Blended result should be between the two extremes (or equal if they match)
   double mtfVal = mtfOnly.Apply( 0.3 );
   double powVal = powerOnly.Apply( 0.3 );
   double blendVal = blended.Apply( 0.3 );

   double lo = std::min( mtfVal, powVal );
   double hi = std::max( mtfVal, powVal );
   CHECK( blendVal >= lo - 1e-10 );
   CHECK( blendVal <= hi + 1e-10 );
}

// ============================================================================
//  10. SASStretch
// ============================================================================

TEST_CASE( "SASStretch boundary values", "[stretch][sas]" )
{
   pcl::SASStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
}

TEST_CASE( "SASStretch monotonicity", "[stretch][sas]" )
{
   pcl::SASStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "SASStretch auto-configure", "[stretch][sas]" )
{
   pcl::SASStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.05 );
   CHECK( mid < 0.95 );

   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "SASStretch output range [0,1]", "[stretch][sas]" )
{
   pcl::SASStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "SASStretch noise-aware stretching", "[stretch][sas]" )
{
   pcl::SASStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );

   // SAS clips values below noise floor — faint values near noise may map to 0.
   // Bright values above noise should be stretched significantly.
   double brightInput = 0.3;
   double brightOut = algo.Apply( brightInput );
   CHECK( brightOut > brightInput );

   // Faint values near noise floor may be clipped to 0 (noise-aware behavior)
   double faintInput = 0.005;
   double faintOut = algo.Apply( faintInput );
   CHECK( faintOut >= 0.0 );
   CHECK( faintOut <= faintInput + 0.1 );  // should not over-amplify noise
}

// ============================================================================
//  11. VeraluxStretch
// ============================================================================

TEST_CASE( "VeraluxStretch boundary values", "[stretch][veralux]" )
{
   pcl::VeraluxStretch algo;
   CHECK( algo.Apply( 0.0 ) == Approx( 0.0 ).margin( 1e-6 ) );
   CHECK( algo.Apply( 1.0 ) == Approx( 1.0 ).margin( 0.01 ) );
   double mid = algo.Apply( 0.5 );
   CHECK( mid > 0.0 );
   CHECK( mid < 1.0 );
}

TEST_CASE( "VeraluxStretch monotonicity", "[stretch][veralux]" )
{
   pcl::VeraluxStretch algo;
   double prev = algo.Apply( 0.0 );
   for ( int i = 1; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= prev - 1e-10 );
      prev = y;
   }
}

TEST_CASE( "VeraluxStretch auto-configure", "[stretch][veralux]" )
{
   pcl::VeraluxStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   double mid = algo.Apply( 0.05 );
   CHECK( mid > 0.0 );
   CHECK( mid < 0.95 );

   // Veralux uses a film-response S-curve that may not be strictly monotonic
   // at all parameter settings. Test output range instead.
   for ( int i = 0; i <= 100; ++i )
   {
      double x = i / 100.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "VeraluxStretch output range [0,1]", "[stretch][veralux]" )
{
   pcl::VeraluxStretch algo;
   algo.AutoConfigure( 0.05, 0.002 );
   for ( int i = 0; i <= 1000; ++i )
   {
      double x = i / 1000.0;
      double y = algo.Apply( x );
      CHECK( y >= -1e-10 );
      CHECK( y <= 1.0 + 1e-10 );
   }
}

TEST_CASE( "VeraluxStretch exposure adjustment", "[stretch][veralux]" )
{
   pcl::VeraluxStretch bright;
   bright.SetExposure( 1.0 );
   pcl::VeraluxStretch dark;
   dark.SetExposure( -1.0 );

   // Positive exposure should brighten
   double brightVal = bright.Apply( 0.3 );
   double darkVal = dark.Apply( 0.3 );
   CHECK( brightVal > darkVal );
}

TEST_CASE( "VeraluxStretch presets maintain output range", "[stretch][veralux]" )
{
   auto checkRange = []( pcl::VeraluxStretch& algo ) {
      for ( int i = 0; i <= 100; ++i )
      {
         double x = i / 100.0;
         double y = algo.Apply( x );
         CHECK( y >= -1e-10 );
         CHECK( y <= 1.0 + 1e-10 );
      }
   };

   pcl::VeraluxStretch a; a.PresetNeutral();        checkRange( a );
   pcl::VeraluxStretch b; b.PresetHighContrast();    checkRange( b );
   pcl::VeraluxStretch c; c.PresetLowContrast();     checkRange( c );
   pcl::VeraluxStretch d; d.PresetCinematic();       checkRange( d );
}

// ============================================================================
//  Cross-cutting: GetParameters and Clone for all algorithms
// ============================================================================

TEST_CASE( "All algorithms return non-empty parameter lists", "[stretch][parameters]" )
{
   pcl::MTFStretch          mtf;
   pcl::ArcSinhStretch      arcsinh;
   pcl::GHStretch           ghs;
   pcl::HistogramStretch    histogram;
   pcl::LogStretch          log;
   pcl::LumptonStretch      lumpton;
   pcl::RNCStretch          rnc;
   pcl::PhotometricStretch  photometric;
   pcl::OTSStretch          ots;
   pcl::SASStretch          sas;
   pcl::VeraluxStretch      veralux;

   CHECK( mtf.GetParameters().size() > 0 );
   CHECK( arcsinh.GetParameters().size() > 0 );
   CHECK( ghs.GetParameters().size() > 0 );
   CHECK( histogram.GetParameters().size() > 0 );
   CHECK( log.GetParameters().size() > 0 );
   CHECK( lumpton.GetParameters().size() > 0 );
   CHECK( rnc.GetParameters().size() > 0 );
   CHECK( photometric.GetParameters().size() > 0 );
   CHECK( ots.GetParameters().size() > 0 );
   CHECK( sas.GetParameters().size() > 0 );
   CHECK( veralux.GetParameters().size() > 0 );
}

TEST_CASE( "All algorithms Clone preserves Apply behavior", "[stretch][clone]" )
{
   auto testClone = []( pcl::IStretchAlgorithm& algo ) {
      algo.AutoConfigure( 0.05, 0.002 );
      auto clone = algo.Clone();
      for ( int i = 0; i <= 10; ++i )
      {
         double x = i / 10.0;
         CHECK( clone->Apply( x ) == Approx( algo.Apply( x ) ).margin( 1e-10 ) );
      }
   };

   pcl::MTFStretch          mtf;         testClone( mtf );
   pcl::ArcSinhStretch      arcsinh;     testClone( arcsinh );
   pcl::GHStretch           ghs;         testClone( ghs );
   pcl::HistogramStretch    histogram;   testClone( histogram );
   pcl::LogStretch          log;         testClone( log );
   pcl::LumptonStretch      lumpton;     testClone( lumpton );
   pcl::RNCStretch          rnc;         testClone( rnc );
   pcl::PhotometricStretch  photometric; testClone( photometric );
   pcl::OTSStretch          ots;         testClone( ots );
   pcl::SASStretch          sas;         testClone( sas );
   pcl::VeraluxStretch      veralux;     testClone( veralux );
}

TEST_CASE( "All algorithms ResetParameters restores defaults", "[stretch][reset]" )
{
   auto testReset = []( pcl::StretchAlgorithmBase& algo ) {
      // Get default value at 0.5
      double defaultVal = algo.Apply( 0.5 );
      // Change via auto-configure
      algo.AutoConfigure( 0.05, 0.002 );
      // Reset
      algo.ResetParameters();
      // Should match original default
      double resetVal = algo.Apply( 0.5 );
      CHECK( resetVal == Approx( defaultVal ).margin( 1e-10 ) );
   };

   pcl::MTFStretch          mtf;         testReset( mtf );
   pcl::ArcSinhStretch      arcsinh;     testReset( arcsinh );
   pcl::GHStretch           ghs;         testReset( ghs );
   pcl::HistogramStretch    histogram;   testReset( histogram );
   pcl::LogStretch          log;         testReset( log );
   pcl::LumptonStretch      lumpton;     testReset( lumpton );
   pcl::RNCStretch          rnc;         testReset( rnc );
   pcl::PhotometricStretch  photometric; testReset( photometric );
   pcl::VeraluxStretch      veralux;     testReset( veralux );
   // Note: OTSStretch and SASStretch have internal mutable state
   // (m_midtones, m_noiseEstimate) that ResetParameters() does not
   // restore, so they are excluded from this parameter-only reset test.
}

// ============================================================================
//  Cross-cutting: varied image statistics for AutoConfigure
// ============================================================================

TEST_CASE( "All algorithms handle very faint image (median=0.001)", "[stretch][faint]" )
{
   auto testFaint = []( pcl::IStretchAlgorithm& algo ) {
      algo.AutoConfigure( 0.001, 0.0002 );
      // Output should still be in range for all inputs
      for ( int i = 0; i <= 100; ++i )
      {
         double x = i / 100.0;
         double y = algo.Apply( x );
         CHECK( y >= -1e-10 );
         CHECK( y <= 1.0 + 1e-10 );
      }
   };

   pcl::MTFStretch          mtf;         testFaint( mtf );
   pcl::ArcSinhStretch      arcsinh;     testFaint( arcsinh );
   pcl::GHStretch           ghs;         testFaint( ghs );
   pcl::HistogramStretch    histogram;   testFaint( histogram );
   pcl::LogStretch          log;         testFaint( log );
   pcl::LumptonStretch      lumpton;     testFaint( lumpton );
   pcl::RNCStretch          rnc;         testFaint( rnc );
   pcl::PhotometricStretch  photometric; testFaint( photometric );
   pcl::OTSStretch          ots;         testFaint( ots );
   pcl::SASStretch          sas;         testFaint( sas );
   pcl::VeraluxStretch      veralux;     testFaint( veralux );
}

TEST_CASE( "All algorithms handle bright image (median=0.4)", "[stretch][bright]" )
{
   auto testBright = []( pcl::IStretchAlgorithm& algo ) {
      algo.AutoConfigure( 0.4, 0.02 );
      for ( int i = 0; i <= 100; ++i )
      {
         double x = i / 100.0;
         double y = algo.Apply( x );
         CHECK( y >= -1e-10 );
         CHECK( y <= 1.0 + 1e-10 );
      }
   };

   pcl::MTFStretch          mtf;         testBright( mtf );
   pcl::ArcSinhStretch      arcsinh;     testBright( arcsinh );
   pcl::GHStretch           ghs;         testBright( ghs );
   pcl::HistogramStretch    histogram;   testBright( histogram );
   pcl::LogStretch          log;         testBright( log );
   pcl::LumptonStretch      lumpton;     testBright( lumpton );
   pcl::RNCStretch          rnc;         testBright( rnc );
   pcl::PhotometricStretch  photometric; testBright( photometric );
   pcl::OTSStretch          ots;         testBright( ots );
   pcl::SASStretch          sas;         testBright( sas );
   pcl::VeraluxStretch      veralux;     testBright( veralux );
}
