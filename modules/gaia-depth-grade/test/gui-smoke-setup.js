// GaiaDepthGrade GUI smoke, start-up (test/gui-smoke.sh; -r at start-up). Test-only, never shipped.
//
// Makes a small image window gdg_gui_dummy (shown, so it is the active window the interface
// takes as its default target), then opens the GaiaDepthGrade interface from a default
// instance, with the Process Console hidden so nothing overlaps it. The script ENDS so the GUI
// is idle (PixInsight disables interfaces while a script runs) for the driver's clicks, and
// writes $GDG_GUI_DIR/ready.
//
// ES5 only: an ES6 `class` anywhere kills a -r= script before line 1 runs.

#include <pjsr/UndoFlag.jsh>

var DIR = getEnvironmentVariable( "GDG_GUI_DIR" );

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
   Console.hide( false );
   var w = new ImageWindow( 64, 64, 1, 32, true, false, "gdg_gui_dummy" );
   w.mainView.beginProcess( UndoFlag_NoSwapFile );
   w.mainView.image.fill( 0.25 );
   w.mainView.endProcess();
   w.show();
   w.zoomToFit();
   log.push( "dummy=" + w.mainView.id + " active=" + ImageWindow.activeWindow.mainView.id );
   var P = new GaiaDepthGrade;
   log.push( "launchInterface=" + P.launchInterface() );
   pump( 1500 );
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
File.writeTextFile( DIR + "/ready", log.join( "\n" ) );
