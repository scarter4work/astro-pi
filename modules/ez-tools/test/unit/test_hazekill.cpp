// EZHazeKill engine vs the hk_lib.hk_v11 goldens (make_golden.py) and the model's invariants.

#include <catch_amalgamated.hpp>

#include "MiniJson.h"
#include "Synth.h"
#include "hazekill/HazeKill.h"

#include <cmath>
#include <limits>
#include <numeric>

using namespace eztools::hazekill;
using eztools::test::Json;
using eztools::test::LoadJson;
using eztools::test::Synth;
using eztools::test::SynthImage;

namespace
{

const Json& Goldens()
{
   static const Json j = LoadJson( std::string( EZTOOLS_TEST_DIR ) + "/golden/hazekill_golden.json" );
   return j;
}

SynthImage SynthCase( const Json& c )
{
   return Synth( uint64_t( c["seed"].Num() ), int( c["width"].Num() ), int( c["height"].Num() ), c["sky_in"].Nums(),
                 c["sigma_in"].Num(), c["blob"].Num(), c["kind"].str, Goldens()["blob_sigma"].Num() );
}

const Json& Case( const std::string& name )
{
   const Json& cs = Goldens()["cases"];
   for ( size_t i = 0; i < cs.size(); ++i )
      if ( cs[i]["name"].str == name )
         return cs[i];
   throw std::runtime_error( "no golden case " + name );
}

std::vector<const double*> CPtrs( const SynthImage& im )
{
   std::vector<const double*> p;
   for ( const auto& ch : im.data )
      p.push_back( ch.data() );
   return p;
}

std::vector<double*> Ptrs( SynthImage& im )
{
   std::vector<double*> p;
   for ( auto& ch : im.data )
      p.push_back( ch.data() );
   return p;
}

Model ModelFor( const SynthImage& im, const Settings& s = Settings() )
{
   SkyEstimate e = MeasureSky( CPtrs( im ), im.width, im.height );
   REQUIRE( e.ok );
   return BuildModel( e, s );
}

// The double path is the reference's own arithmetic (hk_lib runs in float64 on these inputs):
// it must agree to rounding, not to a loose tolerance.
constexpr double kTolModelDouble = 1e-12;
constexpr double kTolSampleDouble = 1e-12;
// The float path (float32 images): inputs rounded to float, output written as float.
constexpr double kTolModelFloat = 1e-7;
constexpr double kTolSampleFloat = 2.5e-7;

void CheckGolden( const std::string& name )
{
   const Json& c = Case( name );
   SynthImage im = SynthCase( c );
   Settings s{ c["amount"].Num(), c["neutralize"].Num(), c["target"].Num() };

   // double path
   {
      Model m = ModelFor( im, s );
      auto sky = c["sky"].Nums(), sigma = c["sigma"].Nums(), offset = c["offset"].Nums();
      REQUIRE( m.sky.size() == sky.size() );
      for ( size_t k = 0; k < sky.size(); ++k )
      {
         CHECK( std::fabs( m.sky[k] - sky[k] ) <= kTolModelDouble );
         CHECK( std::fabs( m.sigma[k] - sigma[k] ) <= kTolModelDouble );
         CHECK( std::fabs( m.offset[k] - offset[k] ) <= kTolModelDouble );
      }
      CHECK( std::fabs( m.tEff - c["target_eff"].Num() ) <= kTolModelDouble );
      CHECK( std::fabs( m.ped - c["ped"].Num() ) <= kTolModelDouble );

      SynthImage out = im;
      Apply( Ptrs( out ), size_t( im.width ) * im.height, m );
      const Json& smp = c["samples"];
      double worst = 0;
      size_t plane = size_t( im.width ) * im.height;
      for ( size_t i = 0; i < smp.size(); ++i )
      {
         size_t idx = size_t( smp[i][0].Num() );
         double got = out.data[idx / plane][idx % plane];
         worst = std::max( worst, std::fabs( got - smp[i][1].Num() ) );
         REQUIRE( std::isfinite( got ) );
      }
      INFO( name << " double path: worst sample |d| " << worst );
      CHECK( worst <= kTolSampleDouble );
   }

   // float path
   {
      std::vector<std::vector<float>> f( im.data.size() );
      std::vector<const float*> cp;
      std::vector<float*> p;
      for ( size_t k = 0; k < im.data.size(); ++k )
      {
         f[k].assign( im.data[k].begin(), im.data[k].end() );
         cp.push_back( f[k].data() );
         p.push_back( f[k].data() );
      }
      SkyEstimate e = MeasureSky( cp, im.width, im.height );
      REQUIRE( e.ok );
      Model m = BuildModel( e, s );
      auto offset = c["offset"].Nums();
      for ( size_t k = 0; k < offset.size(); ++k )
         CHECK( std::fabs( m.offset[k] - offset[k] ) <= kTolModelFloat );
      Apply( p, size_t( im.width ) * im.height, m );
      const Json& smp = c["samples"];
      double worst = 0;
      size_t plane = size_t( im.width ) * im.height;
      for ( size_t i = 0; i < smp.size(); ++i )
      {
         size_t idx = size_t( smp[i][0].Num() );
         worst = std::max( worst, std::fabs( double( f[idx / plane][idx % plane] ) - smp[i][1].Num() ) );
      }
      INFO( name << " float path: worst sample |d| " << worst );
      CHECK( worst <= kTolSampleFloat );
   }
}

} // namespace

TEST_CASE( "golden: rgb_sky_object" ) { CheckGolden( "rgb_sky_object" ); }
TEST_CASE( "golden: stretched_like" ) { CheckGolden( "stretched_like" ); }
TEST_CASE( "golden: mono" ) { CheckGolden( "mono" ); }
TEST_CASE( "golden: tiny_40x30_rgb" ) { CheckGolden( "tiny_40x30_rgb" ); }
TEST_CASE( "golden: flat_constant" ) { CheckGolden( "flat_constant" ); }
TEST_CASE( "golden: black_clipped" ) { CheckGolden( "black_clipped" ); }

TEST_CASE( "golden cases are the plan's six" )
{
   CHECK( Goldens()["cases"].size() == 6 );
}

TEST_CASE( "toe is continuous, C1 and positive" )
{
   for ( double eps : { 1e-3, 6e-4, 0.02, 0.1 } )
   {
      CHECK( Toe( eps, eps ) == eps );
      double slope = ( Toe( eps, eps ) - Toe( eps - 1e-9, eps ) ) / 1e-9;
      CHECK( std::fabs( slope - 1 ) <= 1e-6 );
      // Positive wherever exp() does not underflow (u/eps > -744): Toe(-1, eps) > 0 needs eps >
      // ~1.35e-3; below that it is 0, as in hk_lib (the plan's "eps >= 1e-3" was off by the underflow).
      if ( eps >= 2e-3 )
         CHECK( Toe( -1, eps ) > 0 );
      CHECK( Toe( -1, eps ) >= 0 );
      CHECK( Toe( -0.1, eps ) > 0 );   // the realistic range: u >= -(largest offset)
      CHECK( Toe( 0.5 * eps, eps ) < 0.5 * eps + eps );   // below eps it is the toe, not identity
      CHECK( Toe( 0.5 * eps, eps ) == Catch::Approx( eps * std::exp( -0.5 ) ).epsilon( 1e-15 ) );
      CHECK( Toe( 2 * eps, eps ) == 2 * eps );
   }
   // monotonic
   double prev = -1;
   for ( double u = -0.01; u < 0.01; u += 1e-5 )
   {
      double v = Toe( u, 1e-3 );
      CHECK( v >= prev );
      prev = v;
   }
}

TEST_CASE( "never raises the pedestal" )
{
   SynthImage im = Synth( 77, 256, 256, { 0.02, 0.022, 0.018 }, 2e-4, 0.0, "noise" );
   Model m = ModelFor( im, Settings{ 0.5, 1.0, 0.05 } );
   CHECK( m.ped == 0 );
   CHECK( std::fabs( m.offset[0] + m.offset[1] + m.offset[2] ) <= 1e-15 );
   // Amount 1 changes nothing either: there is no pedestal above the target to remove.
   Model m1 = ModelFor( im, Settings{ 1.0, 1.0, 0.05 } );
   CHECK( m1.ped == 0 );
}

TEST_CASE( "amount 0 neutralize 0 changes only pixels below eps" )
{
   SynthImage im = Synth( 1, 640, 448, { 0.03, 0.035, 0.028 }, 3e-4, 0.2, "noise" );
   // push a few samples below eps and above 1 to exercise the toe and the clamp
   im.data[0][10] = -0.01;
   im.data[1][20] = 0.0;
   im.data[2][30] = 1e-5;
   Model m = ModelFor( im, Settings{ 0.0, 0.0, 0.05 } );
   for ( double o : m.offset )
      CHECK( o == 0 );
   SynthImage out = im;
   Apply( Ptrs( out ), size_t( im.width ) * im.height, m );
   size_t changed = 0;
   for ( size_t c = 0; c < im.data.size(); ++c )
      for ( size_t i = 0; i < im.data[c].size(); ++i )
      {
         double x = im.data[c][i];
         if ( x >= m.eps && x <= 1 )
            REQUIRE( out.data[c][i] == x );
         else
         {
            ++changed;
            CHECK( out.data[c][i] > 0 );
         }
      }
   CHECK( changed == 3 );
}

TEST_CASE( "NaN refused" )
{
   SynthImage im = Synth( 1, 128, 128, { 0.03 }, 3e-4, 0.0, "noise" );
   im.data[0][500] = std::numeric_limits<double>::quiet_NaN();
   SkyEstimate e = MeasureSky( CPtrs( im ), im.width, im.height );
   CHECK_FALSE( e.ok );
   CHECK( e.error.find( "NaN or infinite" ) != std::string::npos );

   im.data[0][500] = std::numeric_limits<double>::infinity();
   CHECK_FALSE( MeasureSky( CPtrs( im ), im.width, im.height ).ok );

   std::vector<float> f( 64 * 64, 0.1f );
   f[7] = -std::numeric_limits<float>::infinity();
   SkyEstimate ef = MeasureSky( std::vector<const float*>{ f.data() }, 64, 64 );
   CHECK_FALSE( ef.ok );
   CHECK( ef.error.find( "NaN or infinite" ) != std::string::npos );
}

TEST_CASE( "integer images are measured in place, as their [0,1] doubles" )
{
   // 8/16/32-bit samples are read as x * (1/max) (PixInsight's FromSample), tile by tile, with no
   // whole-image double copy: the estimate equals the one on the converted double image, exactly.
   SynthImage im = SynthCase( Case( "rgb_sky_object" ) );
   const size_t n = size_t( im.width ) * im.height;
   auto check = [&]( auto zero, double maxv )
   {
      using T = decltype( zero );
      std::vector<std::vector<T>> q( 3, std::vector<T>( n ) );
      SynthImage d = im;
      for ( size_t c = 0; c < 3; ++c )
         for ( size_t i = 0; i < n; ++i )
         {
            double v = std::min( 1.0, std::max( 0.0, im.data[c][i] ) );
            q[c][i] = T( std::llround( v * maxv ) );
            d.data[c][i] = double( q[c][i] ) * ( 1.0 / maxv );
         }
      std::vector<const T*> p{ q[0].data(), q[1].data(), q[2].data() };
      SkyEstimate ei = MeasureSky( p, im.width, im.height );
      SkyEstimate ed = MeasureSky( CPtrs( d ), d.width, d.height );
      REQUIRE( ei.ok );
      REQUIRE( ed.ok );
      for ( size_t c = 0; c < 3; ++c )
      {
         CHECK( ei.sky[c] == ed.sky[c] );
         CHECK( ei.sigma[c] == ed.sigma[c] );
      }
      CHECK( ei.skyTiles == ed.skyTiles );
   };
   check( uint8_t( 0 ), 255.0 );
   check( uint16_t( 0 ), 65535.0 );
   check( uint32_t( 0 ), 4294967295.0 );
}

TEST_CASE( "empty input refused" )
{
   CHECK_FALSE( MeasureSky( std::vector<const double*>{}, 10, 10 ).ok );
   std::vector<double> d( 1, 0.1 );
   CHECK_FALSE( MeasureSky( std::vector<const double*>{ d.data() }, 0, 10 ).ok );
}

TEST_CASE( "stretched flag" )
{
   CHECK( ModelFor( SynthCase( Case( "stretched_like" ) ) ).stretched );
   CHECK_FALSE( ModelFor( SynthCase( Case( "rgb_sky_object" ) ) ).stretched );
}

TEST_CASE( "tiny image is one tile, never NaN" )
{
   Model m = ModelFor( SynthCase( Case( "tiny_40x30_rgb" ) ) );
   CHECK( m.tiles == 1 );
   CHECK( m.skyTiles == 1 );
   Model f = ModelFor( SynthCase( Case( "flat_constant" ) ) );
   CHECK( f.eps == 1e-6 );
   CHECK( f.floorLevel == 0 );
}

TEST_CASE( "report: the script's hkReport text" )
{
   Model m = ModelFor( SynthCase( Case( "rgb_sky_object" ) ) );
   std::string r = Report( m );
   CHECK( r.rfind( "Sky from the darkest 10% of 70 tiles (", 0 ) == 0 );
   CHECK( r.find( "\n   R: sky 0.02999" ) != std::string::npos );
   CHECK( r.find( "\n   G: sky " ) != std::string::npos );
   CHECK( r.find( "4-sigma noise floor" ) != std::string::npos );
   CHECK( r.find( "The sky is already at or below the target: the pedestal is left as it is (never raised)." ) != std::string::npos );
   CHECK( r.find( "Color cast removal: 100% (Neutralize 1.00)." ) != std::string::npos );
   CHECK( r.find( "Note:" ) == std::string::npos );
   CHECK( r.back() != '\n' );

   Model s = ModelFor( SynthCase( Case( "stretched_like" ) ) );
   std::string rs = Report( s );
   CHECK( rs.find( "Pedestal above the target: " ) != std::string::npos );
   CHECK( rs.find( "removing 50% of it (Amount 0.50)." ) != std::string::npos );
   size_t note = rs.rfind( "\nNote: the sky noise is " );
   REQUIRE( note != std::string::npos );
   CHECK( rs.find( "looks like a stretched (non-linear) image", note ) != std::string::npos );

   Model k = ModelFor( SynthCase( Case( "mono" ) ) );
   CHECK( Report( k ).find( "\n   K: sky " ) != std::string::npos );
}
