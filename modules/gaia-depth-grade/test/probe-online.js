// GaiaDepthGrade probe `online_live` (plan Task 13). Test-only, never shipped. LIVE: it queries
// the ESA Gaia archive (https://gea.esac.esa.int/) once, through the module's TapClient
// (pcl::NetworkTransfer), into a private cache directory, then re-runs from the cache.
// Env: GDG_PROBE_OUT, GDG_ONLINE_ARGS ("<ra> <dec> <radius_deg> <magLimit> <cacheDir>").
//
// ES5 only (an ES6 class anywhere kills a -r= script before line 1 runs).

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );
try
{
   var P = new GaiaDepthGradeProbe;
   P.probeCase = "online";
   P.probeArgs = getEnvironmentVariable( "GDG_ONLINE_ARGS" );
   P.outputPath = OUT;
   P.executeGlobal();
}
catch ( e )
{
   File.writeTextFile( OUT, JSON.stringify( { ok: false, error: "probe-online.js: " + e.toString() } ) + "\n" );
}
