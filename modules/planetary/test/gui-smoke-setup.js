// Planetary GUI smokes, step 1 (test/gui-smoke.sh, test/gui-smoke-stack.sh;
// -r at start-up): the interface of process $PLANETARY_GUI_PROCESS (SERViewer
// or PlanetaryStack) opened empty, the Process Console hidden so nothing
// overlaps it. The script ENDS so the GUI is idle (PixInsight disables
// interfaces while a script runs) for the driver's clicks. Test-only.
//
// ES5 only: an ES6 `class` anywhere kills a -r= script before line 1 runs.

var DIR = getEnvironmentVariable( "PLANETARY_GUI_DIR" );
var PROCESS = getEnvironmentVariable( "PLANETARY_GUI_PROCESS" );

function pump( ms )
{
   var t0 = Date.now();
   while ( Date.now() - t0 < ms )
   {
      processEvents();
      msleep( 20 );
   }
}

var log = [];
try
{
   if ( PROCESS != "SERViewer" && PROCESS != "PlanetaryStack" )
      throw new Error( "PLANETARY_GUI_PROCESS must be SERViewer or PlanetaryStack, got '" + PROCESS + "'" );
   Console.hide( false );
   var P = (PROCESS == "SERViewer") ? new SERViewer : new PlanetaryStack;
   log.push( PROCESS + " filePath='" + P.filePath + "'" );
   log.push( "launchInterface=" + P.launchInterface() );
   pump( 1500 );
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
File.writeTextFile( DIR + "/ready", log.join( "\n" ) );
