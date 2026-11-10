// TEST-ONLY probe case `local_gaia` (plan Task 12).
//
// probeArgs = "<ra> <dec> <radius_deg> <magLimit> <rowsFile>". Discovers the Gaia database
// files, runs LocalGaiaSource::Search on the cone (rows written to <rowsFile> as TSV with %.17g:
// ra dec parx magG magBP magRP flags) and distances_for (counts, BP-RP pass-through, log lines).
// probe-local-gaia.js compares the rows with the Gaia process' own search.
#include "ProbeJson.h"
#include "ProbeProcess.h"

#include "../LocalGaia.h"

#include <pcl/ElapsedTime.h>
#include <pcl/ErrorHandler.h>
#include <pcl/File.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pcl
{

using namespace gdgprobe;

IsoString ProbeLocalGaia( const String& args )
{
   std::istringstream in( args.ToUTF8().c_str() );
   gdg::Footprint fp;
   double mag;
   std::string rowsFile;
   if ( !(in >> fp.center_ra >> fp.center_dec >> fp.radius_deg >> mag >> rowsFile) )
      throw Error( "local_gaia: probeArgs must be '<ra> <dec> <radius_deg> <magLimit> <rowsFile>'" );

   const StringList paths = DiscoverGaiaDatabases();
   std::vector<std::string> info, warning;
   const gdg::Log log{ [&]( const std::string& m ) { info.push_back( m ); },
                       [&]( const std::string& m ) { warning.push_back( m ); } };
   LocalGaiaSource src( paths, mag, 5.0, log );

   ElapsedTime T;
   const std::vector<LocalGaiaStar> stars = src.Search( fp );
   const double searchSeconds = T();

   IsoString tsv;
   char line[ 512 ];
   for ( const LocalGaiaStar& s : stars )
   {
      std::snprintf( line, sizeof( line ), "%.17g\t%.17g\t%.17g\t%.17g\t%.17g\t%.17g\t%u\n",
                     s.ra, s.dec, s.parx, s.magG, s.magBP, s.magRP, unsigned( s.flags ) );
      tsv += line;
   }
   File::WriteTextFile( String::UTF8ToUTF16( rowsFile.c_str() ), tsv );

   const gdg::Catalog cat = src.distances_for( fp );
   // BP-RP pass-through: every catalogue row carries the bp_rp of its database star.
   std::map<std::pair<double, double>, double> bpOf;
   size_type starsWithColour = 0;
   for ( const LocalGaiaStar& s : stars )
   {
      bpOf[ { s.ra, s.dec } ] = LocalGaiaSource::BPRP( s );
      if ( std::isfinite( LocalGaiaSource::BPRP( s ) ) )
         ++starsWithColour;
   }
   size_type bpFinite = 0, bpMismatch = 0;
   for ( const gdg::CatalogRow& r : cat )
   {
      if ( std::isfinite( r.bp_rp ) )
         ++bpFinite;
      auto it = bpOf.find( { r.ra, r.dec } );
      const bool same = it != bpOf.end() &&
                        ((std::isnan( it->second ) && std::isnan( r.bp_rp )) || it->second == r.bp_rp);
      if ( !same )
         ++bpMismatch;
   }

   IsoString p = "[";
   for ( size_type i = 0; i < paths.Length(); ++i )
      p += (i ? "," : "") + Str( paths[i] );
   p += "]";
   IsoString w = "[";
   for ( size_t i = 0; i < warning.size(); ++i )
      w += (i ? "," : "") + Str( String::UTF8ToUTF16( warning[i].c_str() ) );
   w += "]";
   IsoString n = "[";
   for ( size_t i = 0; i < info.size(); ++i )
      n += (i ? "," : "") + Str( String::UTF8ToUTF16( info[i].c_str() ) );
   n += "]";

   return Field( "paths", p ) + "," + Field( "rowsFile", Str( String::UTF8ToUTF16( rowsFile.c_str() ) ) ) + "," +
          Field( "nStars", Num( double( stars.size() ) ) ) + "," +
          Field( "starsWithColour", Num( double( starsWithColour ) ) ) + "," +
          Field( "searchSeconds", Num( searchSeconds ) ) + "," +
          Field( "nCatalog", Num( double( cat.size() ) ) ) + "," +
          Field( "catalogBpFinite", Num( double( bpFinite ) ) ) + "," +
          Field( "catalogBpMismatch", Num( double( bpMismatch ) ) ) + "," +
          Field( "warnings", w ) + "," + Field( "infos", n );
}

} // namespace pcl
