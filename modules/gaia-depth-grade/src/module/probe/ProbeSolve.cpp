// TEST-ONLY probe case `solve` (plan Tasks 14-15): runs the module's native plate solver
// (SolveImage) on an open image window and reports what it did. The driving script
// (test/probe-solve.js) compares the solution PixInsight regenerates from what the solver wrote
// with ImageSolver's own (test/solver-reference.js).
//
// probeArgs: space-separated key=value pairs
//   window=<id>          the image window to solve (required)
//   gaia=<a:b:...>       Gaia DR3 .xpsd files (default: DiscoverGaiaDatabases)
//   index=<path>         astrometric index (default: the Gaia process' configured one)
//   blind=auto|always|disabled
//   distortion=1|0
//   existing=1|0         seed from an existing astrometric solution
#include "ProbeJson.h"
#include "ProbeProcess.h"

#include "../Solver.h"

#include <pcl/ErrorHandler.h>
#include <pcl/ImageWindow.h>

#include <string>
#include <vector>

namespace pcl
{

using namespace gdgprobe;

IsoString ProbeSolve( const String& args )
{
   StringList tokens;
   args.Break( tokens, ' ', true/*trim*/ );
   String windowId;
   SolverParams p;
   for ( const String& t : tokens )
   {
      if ( t.IsEmpty() )
         continue;
      const size_type eq = t.Find( '=' );
      if ( eq == String::notFound )
         throw Error( "solve: malformed argument '" + t + "'" );
      const String k = t.Left( eq ), v = t.Substring( eq + 1 );
      if ( k == "window" )
         windowId = v;
      else if ( k == "gaia" )
      {
         StringList files;
         v.Break( files, ':', true );
         for ( const String& f : files )
            if ( !f.IsEmpty() )
               p.gaiaDatabaseFiles << f;
      }
      else if ( k == "index" )
         p.indexFile = v;
      else if ( k == "blind" )
         p.blind = (v == "always") ? BlindMode::Always : (v == "disabled") ? BlindMode::Disabled : BlindMode::Automatic;
      else if ( k == "distortion" )
         p.distortion = v != "0";
      else if ( k == "existing" )
         p.useExistingSolution = v != "0";
      else
         throw Error( "solve: unknown argument '" + k + "'" );
   }
   ImageWindow window = ImageWindow::WindowById( IsoString( windowId ) );
   if ( window.IsNull() )
      throw Error( "solve: no window '" + windowId + "'" );

   std::vector<std::string> infos, warnings;
   const gdg::Log log{ [&]( const std::string& s ) { infos.push_back( s ); },
                       [&]( const std::string& s ) { warnings.push_back( s ); } };
   const SolveReport r = SolveImage( window, p, log );

   auto list = []( const std::vector<std::string>& v )
   {
      IsoString s = "[";
      for ( size_t i = 0; i < v.size(); ++i )
         s += (i ? "," : "") + Str( String::UTF8ToUTF16( v[i].c_str() ) );
      return s + "]";
   };
   return Field( "solved", r.ok ? "true" : "false" ) + "," +
          Field( "method", Str( r.method ) ) + "," +
          Field( "error", Str( r.error ) ) + "," +
          Field( "detected", IsoString( r.detected ) ) + "," +
          Field( "matches", IsoString( r.matches ) ) + "," +
          Field( "rmsPx", Num( r.rmsPx ) ) + "," +
          Field( "limitMagnitude", Num( r.limitMagnitude ) ) + "," +
          Field( "catalogStars", IsoString( r.catalogStars ) ) + "," +
          Field( "iterations", IsoString( r.iterations ) ) + "," +
          Field( "seconds", Num( r.seconds ) ) + "," +
          Field( "blindSeconds", Num( r.blindSeconds ) ) + "," +
          Field( "blindAttempt", IsoString( r.blindAttempt ) ) + "," +
          Field( "configuredIndex", Str( ConfiguredAstrometricIndex() ) ) + "," +
          Field( "infos", list( infos ) ) + "," +
          Field( "warnings", list( warnings ) );
}

} // namespace pcl
