// SkyCache: the RTP measures the sky once per view (full image) and re-measures only on a change.

#include <catch_amalgamated.hpp>

#include "hazekill/SkyCache.h"

using namespace eztools::hazekill;

namespace
{

SkyEstimate Fake( double level )
{
   SkyEstimate e;
   e.ok = true;
   e.sky = { level };
   e.sigma = { 0.001 };
   return e;
}

} // namespace

TEST_CASE( "same key measures once" )
{
   SkyCache cache;
   SkyKey k{ "img", 640, 480, 3 };
   for ( int i = 0; i < 5; ++i )
      CHECK( cache.Get( k, [] { return Fake( 0.1 ); } ).sky[0] == 0.1 );
   CHECK( cache.Measurements() == 1 );
}

TEST_CASE( "size change re-measures" )
{
   SkyCache cache;
   cache.Get( SkyKey{ "img", 640, 480, 3 }, [] { return Fake( 0.1 ); } );
   CHECK( cache.Get( SkyKey{ "img", 320, 480, 3 }, [] { return Fake( 0.2 ); } ).sky[0] == 0.2 );
   CHECK( cache.Get( SkyKey{ "img", 320, 480, 1 }, [] { return Fake( 0.3 ); } ).sky[0] == 0.3 );
   CHECK( cache.Measurements() == 3 );
   // the latest measurement for a view replaces the older one (no stale entry comes back)
   CHECK( cache.Get( SkyKey{ "img", 640, 480, 3 }, [] { return Fake( 0.4 ); } ).sky[0] == 0.4 );
   CHECK( cache.Measurements() == 4 );
}

TEST_CASE( "Invalidate(main) also drops its previews" )
{
   SkyCache cache;
   SkyKey main{ "img", 640, 480, 3 }, prev{ "img->Preview01", 100, 80, 3 };
   cache.Get( main, [] { return Fake( 0.1 ); } );
   cache.Get( prev, [] { return Fake( 0.2 ); } );
   CHECK( cache.Measurements() == 2 );
   cache.Invalidate( "img" );
   cache.Get( main, [] { return Fake( 0.1 ); } );
   cache.Get( prev, [] { return Fake( 0.2 ); } );
   CHECK( cache.Measurements() == 4 );
}

TEST_CASE( "other views untouched" )
{
   SkyCache cache;
   SkyKey a{ "img", 640, 480, 3 }, b{ "img2", 640, 480, 3 }, c{ "img2->Preview01", 10, 10, 3 };
   cache.Get( a, [] { return Fake( 0.1 ); } );
   cache.Get( b, [] { return Fake( 0.2 ); } );
   cache.Get( c, [] { return Fake( 0.3 ); } );
   cache.Invalidate( "img" );      // "img2" starts with "img" but is another view
   cache.Get( b, [] { return Fake( 0.2 ); } );
   cache.Get( c, [] { return Fake( 0.3 ); } );
   CHECK( cache.Measurements() == 3 );
   cache.Get( a, [] { return Fake( 0.1 ); } );
   CHECK( cache.Measurements() == 4 );
}

TEST_CASE( "Clear drops everything; a failed measurement is cached too" )
{
   SkyCache cache;
   SkyKey a{ "img", 64, 64, 1 };
   SkyEstimate bad;
   bad.error = "the image contains NaN or infinite samples";
   CHECK_FALSE( cache.Get( a, [&] { return bad; } ).ok );
   CHECK_FALSE( cache.Get( a, [&] { return bad; } ).ok );
   CHECK( cache.Measurements() == 1 );
   cache.Clear();
   CHECK( cache.Get( a, [] { return Fake( 0.1 ); } ).ok );
   CHECK( cache.Measurements() == 2 );
}

TEST_CASE( "an invalidation during a measurement is not overwritten by its stale result" )
{
   SkyCache cache;
   SkyKey a{ "img", 64, 64, 1 };
   // The image changes (ImageUpdated on the GUI thread) while the RTP thread is measuring.
   CHECK( cache.Get( a, [&] { cache.Invalidate( "img" ); return Fake( 0.1 ); } ).sky[0] == 0.1 );
   CHECK( cache.Get( a, [] { return Fake( 0.2 ); } ).sky[0] == 0.2 );
   CHECK( cache.Measurements() == 2 );
}
