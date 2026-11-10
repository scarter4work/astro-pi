// EZTools GUI smoke, step 1 (test/gui-smoke.sh; -r at start-up): open $EZT_GUI_INPUT, give it a
// linked auto-STF (a linear image is black without one, in the view and in the Real-Time Preview),
// hide the Process Console and open the EZHazeKill interface. The script ENDS so the GUI is idle
// (PixInsight disables interfaces while a script runs) for the driver's clicks. Test-only.
// ES5 only.

#include "gui-smoke-common.js"

var log = [];
try
{
   var input = getEnvironmentVariable( "EZT_GUI_INPUT" );
   var ws = ImageWindow.open( input );
   if ( ws.length != 1 )
      throw new Error( "opening " + input + " gave " + ws.length + " windows" );
   var w = ws[0];
   w.mainView.id = "ezt_smoke";
   w.show();
   w.zoomToFit();
   w.bringToFront();
   eztAutoSTF( w.mainView );
   Console.hide( false );
   var P = new EZHazeKill;
   log.push( "launchInterface=" + P.launchInterface() );
   eztPump( 1500 );
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
File.writeTextFile( EZT_GUI_DIR + "/ready", log.join( "\n" ) );
