// GaiaDepthGrade probe `solver_enums` (plan Task 14). Test-only, never shipped.
// StarAlignment's enumeration values on the running PixInsight, which SolverAlign.h hardcodes
// (the C++ API does not publish enumeration element identifiers of core processes).
//
// ES5 only (an ES6 class anywhere kills a -r= script before line 1 runs).

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );
try
{
   var p = StarAlignment.prototype;
   File.writeTextFile( OUT, JSON.stringify( { ok: true, OutputMatrix: p.OutputMatrix, NoIntersection: p.NoIntersection,
                                              Always: p.Always } ) + "\n" );
}
catch ( e )
{
   File.writeTextFile( OUT, JSON.stringify( { ok: false, error: "probe-solver-enums.js: " + e.toString() } ) + "\n" );
}
