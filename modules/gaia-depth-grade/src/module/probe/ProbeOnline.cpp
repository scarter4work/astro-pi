// TEST-ONLY probe case `online` (plan Task 13, run by `online_live` only when GDG_LIVE_TAP=1).
//
// probeArgs = "<ra> <dec> <radius_deg> <magLimit> <cacheDir>". Runs the real online source
// (TapClient over NetworkTransfer to the ESA archive) into a private cache directory, then again,
// which must be answered by the cache with no request at all. Every request and HTTP status is
// recorded.
#include "ProbeJson.h"
#include "ProbeProcess.h"

#include "../TapClient.h"
#include "core/OnlineCache.h"
#include "core/TapText.h"

#include <pcl/ElapsedTime.h>
#include <pcl/ErrorHandler.h>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace pcl
{

using namespace gdgprobe;

namespace
{

class Recording : public gdg::HttpTransport
{
public:
   std::vector<std::string> log;
   double slept = 0;
   gdg::HttpResponse get( const std::string& url ) override { return Note( "GET", url, m.get( url ) ); }
   gdg::HttpResponse post_form( const std::string& url, const std::string& form ) override
   {
      return Note( "POST", url, m.post_form( url, form ) );
   }
   void sleep_seconds( double s ) override { slept += s; m.sleep_seconds( s ); }
private:
   TapClient m;
   gdg::HttpResponse Note( const char* method, const std::string& url, gdg::HttpResponse r )
   {
      log.push_back( std::string( method ) + " " + url + " -> " + std::to_string( r.status ) + " " + r.content_type +
                     (r.error.empty() ? "" : " (" + r.error + ")") );
      return r;
   }
};

IsoString List( const std::vector<std::string>& v )
{
   IsoString s = "[";
   for ( size_t i = 0; i < v.size(); ++i )
      s += (i ? "," : "") + Str( String::UTF8ToUTF16( v[i].c_str() ) );
   return s + "]";
}

} // namespace

IsoString ProbeOnline( const String& args )
{
   std::istringstream in( args.ToUTF8().c_str() );
   gdg::Footprint fp;
   double mag;
   std::string cacheDir;
   if ( !(in >> fp.center_ra >> fp.center_dec >> fp.radius_deg >> mag >> cacheDir) )
      throw Error( "online: probeArgs must be '<ra> <dec> <radius_deg> <magLimit> <cacheDir>'" );

   std::vector<std::string> info, warning;
   const gdg::Log log{ [&]( const std::string& m ) { info.push_back( m ); },
                       [&]( const std::string& m ) { warning.push_back( m ); } };
   const gdg::OnlineCache cache( cacheDir );

   Recording first;
   ElapsedTime T;
   const gdg::Catalog c1 = gdg::online_gaia_distances( first, &cache, fp, mag, log );
   const double seconds = T();
   size_type withColour = 0;
   for ( const gdg::CatalogRow& r : c1 )
      if ( std::isfinite( r.bp_rp ) )
         ++withColour;

   const size_t warningsBefore = warning.size();
   Recording second;
   const gdg::Catalog c2 = gdg::online_gaia_distances( second, &cache, fp, mag, log );
   bool same = c1.size() == c2.size();
   for ( size_t i = 0; same && i < c1.size(); ++i )
      same = c1[i].source_id == c2[i].source_id && c1[i].r_med == c2[i].r_med;
   const std::vector<std::string> secondWarnings( warning.begin() + warningsBefore, warning.end() );

   return Field( "rows", Num( double( c1.size() ) ) ) + "," +
          Field( "rowsWithColour", Num( double( withColour ) ) ) + "," +
          Field( "seconds", Num( seconds ) ) + "," + Field( "slept", Num( first.slept ) ) + "," +
          Field( "requests", List( first.log ) ) + "," +
          Field( "cachedRequests", Num( double( second.log.size() ) ) ) + "," +
          Field( "cachedSame", same ? "true" : "false" ) + "," +
          Field( "cachePath", Str( String::UTF8ToUTF16( cache.path_for( gdg::cache_key( fp, mag ) ).c_str() ) ) ) + "," +
          Field( "cachedWarnings", List( secondWarnings ) ) + "," +
          Field( "warnings", List( std::vector<std::string>( warning.begin(), warning.begin() + warningsBefore ) ) ) + "," +
          Field( "infos", List( info ) );
}

} // namespace pcl
