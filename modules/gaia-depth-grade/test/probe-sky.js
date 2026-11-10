// GaiaDepthGrade probe `sky_adapter_convention` (plan Task 11). Test-only, never shipped.
//
// Opens $GDG_SKY_MASTER (a master already plate-solved by ImageSolver, spline solution) and runs
// the test-only GaiaDepthGradeProbe process on it. The process builds the linear WCS keywords
// the product exported for the Python core and compares PclSky with them (see
// src/module/probe/ProbeSky.cpp); it writes $GDG_PROBE_OUT.
//
// ES5 only (an ES6 class anywhere kills a -r= script before line 1 runs, which is also why the
// WCS export is done in C++ rather than with pjsr/astrometry/AstrometricMetadata.js).

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );
var MASTER = getEnvironmentVariable( "GDG_SKY_MASTER" );

function fail( msg )
{
   File.writeTextFile( OUT, JSON.stringify( { ok: false, error: msg } ) + "\n" );
}

function main()
{
   if ( !OUT )
      throw new Error( "GDG_PROBE_OUT is not set" );
   if ( !MASTER || !File.exists( MASTER ) )
      return fail( "GDG_SKY_MASTER missing or not found: " + MASTER );
   var ws = ImageWindow.open( MASTER );
   if ( ws.length < 1 )
      return fail( "could not open " + MASTER );
   var w = ws[0];
   try
   {
      if ( !w.hasAstrometricSolution )
         return fail( "the master carries no astrometric solution" );
      var P = new GaiaDepthGradeProbe;
      P.probeCase = "sky_adapter_convention";
      P.probeArgs = "";
      P.outputPath = OUT;
      P.executeOn( w.mainView, false );
   }
   finally
   {
      w.forceClose();
   }
}

try
{
   main();
}
catch ( e )
{
   fail( "probe-sky.js: " + e.toString() );
}
