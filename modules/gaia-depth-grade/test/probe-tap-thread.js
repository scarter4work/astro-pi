// GaiaDepthGrade probe `tap_off_main_thread` (plan C14). Test-only, never shipped. No network:
// the module's TapClient is called from std::threads and must refuse before any request.
// Env: GDG_PROBE_OUT.
//
// ES5 only (an ES6 class anywhere kills a -r= script before line 1 runs).

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );
try
{
   var P = new GaiaDepthGradeProbe;
   P.probeCase = "tap_thread";
   P.probeArgs = "";
   P.outputPath = OUT;
   P.executeGlobal();
}
catch ( e )
{
   File.writeTextFile( OUT, JSON.stringify( { ok: false, error: "probe-tap-thread.js: " + e.toString() } ) + "\n" );
}
