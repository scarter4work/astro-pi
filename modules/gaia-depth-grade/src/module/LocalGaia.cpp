#include "LocalGaia.h"

#include "core/Distances.h"
#include "core/Format.h"

#include <pcl/ErrorHandler.h>
#include <pcl/File.h>
#include <pcl/GaiaDatabaseFile.h>
#include <pcl/Settings.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

namespace pcl
{

const char* const kNoLocalGaiaMessage =
   "No local Gaia DR3 database is configured in PixInsight (the one plate-solving uses). Configure it, or uncheck "
   "'Offline' to use the online catalogue.";

LocalGaiaSource::LocalGaiaSource( const StringList& xpsdPaths, std::optional<double> magLimit, double snrMin,
                                  const gdg::Log& log )
   : m_paths( xpsdPaths )
   , m_magLimit( magLimit )
   , m_snrMin( snrMin )
   , m_log( log )
{
   if ( m_paths.IsEmpty() )
      throw Error( kNoLocalGaiaMessage );
}

double LocalGaiaSource::BPRP( const LocalGaiaStar& s )
{
   if ( s.flags & (GaiaStarFlag::NoGBPMag | GaiaStarFlag::NoGRPMag) )
      return NAN;
   return s.magBP - s.magRP;
}

std::vector<LocalGaiaStar> LocalGaiaSource::Search( const gdg::Footprint& fp ) const
{
   std::vector<LocalGaiaStar> out;
   for ( const String& path : m_paths )
   {
      GaiaSearchData data;
      data.centerRA = fp.center_ra;
      data.centerDec = fp.center_dec;
      data.radius = fp.radius_deg;
      data.magnitudeLow = -5;
      if ( m_magLimit )
         data.magnitudeHigh = float( *m_magLimit );
      data.sourceLimit = uint32_max;
      data.requiredFlags = data.inclusionFlags = data.exclusionFlags = 0u;
      try
      {
         GaiaDatabaseFile db( path );
         db.Search( data );
      }
      catch ( const Exception& e )
      {
         throw Error( "local Gaia: cannot search " + path + ": " + e.Message() );
      }
      out.reserve( out.size() + data.stars.Length() );
      for ( const GaiaStarData& s : data.stars )
         out.push_back( LocalGaiaStar{ s.ra, s.dec, double( s.parx ), double( s.magG ), double( s.magBP ),
                                       double( s.magRP ), s.flags } );
   }
   return out;
}

gdg::Catalog LocalGaiaSource::distances_for( const gdg::Footprint& fp )
{
   const std::vector<LocalGaiaStar> stars = Search( fp );
   if ( stars.empty() )
      throw Error( String( gdg::strformat( "local Gaia cone search returned no sources (%d database file(s) searched). "
                                           "The configured .xpsd files may not cover this field, or PI's DR3 / DR3SP "
                                           "database slots are pointed at the wrong files.", int( m_paths.Length() ) ).c_str() ) );
   gdg::log_info( m_log, gdg::strformat( "offline Gaia DR3: %d source(s) from %d database file(s) within %.3f deg of "
                                         "(%.4f, %.4f)", int( stars.size() ), int( m_paths.Length() ), fp.radius_deg,
                                         fp.center_ra, fp.center_dec ) );
   std::vector<double> ra, dec, plx, g, bp;
   ra.reserve( stars.size() ); dec.reserve( stars.size() ); plx.reserve( stars.size() );
   g.reserve( stars.size() ); bp.reserve( stars.size() );
   for ( const LocalGaiaStar& s : stars )
   {
      ra.push_back( s.ra );
      dec.push_back( s.dec );
      plx.push_back( s.parx );
      g.push_back( s.magG );
      bp.push_back( BPRP( s ) );
   }
   return gdg::local_gaia_distances( ra, dec, plx, g, bp, m_magLimit, m_snrMin, m_log );
}

StringList DiscoverGaiaDatabases()
{
#ifdef GDG_PROBES
   // Test builds only: GDG_TEST_NO_LOCAL_GAIA=1 stands for a machine with no local database
   // (discovery reads every settings slot, so a test slot alone cannot hide the user's).
   if ( const char* none = std::getenv( "GDG_TEST_NO_LOCAL_GAIA" ) )
      if ( std::string( none ) == "1" )
         throw Error( kNoLocalGaiaMessage );
#endif
   StringList paths;
   String setting;
   if ( Settings::Read( "GaiaDatabaseFiles", setting ) )
   {
      StringList lines;
      setting.Break( lines, '\n', true/*trim*/ );
      for ( const String& l : lines )
         if ( !l.IsEmpty() )
            paths << l;
      if ( !paths.IsEmpty() )
         return paths;
   }

   namespace fs = std::filesystem;
   const fs::path dir = fs::path( File::HomeDirectory().ToUTF8().c_str() ) / ".PixInsight";
   std::vector<fs::path> files;
   std::error_code ec;
   if ( fs::is_directory( dir, ec ) )
   {
      const std::regex name( "core-[0-9]+-pxi\\.settings" );
      for ( const fs::directory_entry& e : fs::directory_iterator( dir, ec ) )
         if ( e.is_regular_file( ec ) && std::regex_match( e.path().filename().string(), name ) )
            files.push_back( e.path() );
   }
   std::sort( files.begin(), files.end() );
   for ( const fs::path& f : files )
   {
      std::ifstream in( f, std::ios::binary );
      std::stringstream ss;
      ss << in.rdbuf();
      const std::vector<std::string> found = gdg::gaia_db_paths_from_settings( ss.str() );
      if ( !found.empty() )
      {
         for ( const std::string& p : found )
            paths << String::UTF8ToUTF16( p.c_str() );
         return paths;
      }
   }
   throw Error( kNoLocalGaiaMessage );
}

} // namespace pcl
