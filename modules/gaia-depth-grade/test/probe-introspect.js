// GaiaDepthGrade probe `process_category` (plan Task 16). Test-only, never shipped.
// The parameter table (ids, types, defaults, ranges) and categories of the processes named in
// $GDG_INTROSPECT, read by the test-only GaiaDepthGradeProbe through the PCL process API.
//
// ES5 only (an ES6 class anywhere kills a -r= script before line 1 runs).

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );
try
{
   var P = new GaiaDepthGradeProbe;
   P.probeCase = "introspect";
   P.probeArgs = getEnvironmentVariable( "GDG_INTROSPECT" );
   P.outputPath = OUT;
   P.executeGlobal();
}
catch ( e )
{
   File.writeTextFile( OUT, JSON.stringify( { ok: false, error: "probe-introspect.js: " + e.toString() } ) + "\n" );
}
