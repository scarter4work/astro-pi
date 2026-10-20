// Planetary GUI smoke, step 3 (test/gui-smoke.sh; -x IPC into the running
// GUI): launches the SERViewer interface from an instance whose filePath is
// set -- the path a saved process icon takes -- so the interface's
// ImportProcess must load that file and frame. Test-only.
//
// ES5 only: an ES6 `class` anywhere kills a script before line 1 runs.

var DIR = getEnvironmentVariable( "PLANETARY_GUI_DIR" );
var log = [];
try
{
   var P = new SERViewer;
   P.filePath = getEnvironmentVariable( "PLANETARY_GUI_IMPORT_PATH" );
   P.frameIndex = parseInt( getEnvironmentVariable( "PLANETARY_GUI_IMPORT_FRAME" ), 10 );
   P.debayer = true;
   log.push( "filePath='" + P.filePath + "' frameIndex=" + P.frameIndex );
   log.push( "launchInterface=" + P.launchInterface() );
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
File.writeTextFile( DIR + "/imported", log.join( "\n" ) );
