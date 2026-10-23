// PI Copilot quit-in-turn test (test/quit-in-turn.sh), step 1 (-r at start-up):
// a master with History steps (recorded by the production JourneyService: a keep
// needs a journey with steps) as the active image, and the panel shown. The
// script ENDS so the GUI is idle.
#include "gui-smoke-common.jsh"

var log = [];
try
{
   var m = new ImageWindow( 640, 480, 1, 32, true, false, "pcTurnM" );
   m.keywords = [ new FITSKeyword( "IMAGETYP", "'Master Light'", "" ),
                  new FITSKeyword( "OBJECT", "'TurnObj'", "" ),
                  new FITSKeyword( "FILTER", "'Ha'", "" ),
                  new FITSKeyword( "EXPTIME", "300", "" ),
                  new FITSKeyword( "NCOMBINE", "20", "" ) ];
   var p = new PixelMath;
   p.expression = "0.05 + 0.02*sin(x()/5)*cos(y()/7) + 0.01*random()";
   p.executeOn( m.mainView );
   m.show();
   pump( 1500 );
   var steps = [ "$T*1.1", "$T+0.01", "mtf(0.4,$T)" ];
   for ( var i = 0; i < steps.length; ++i )
   {
      var pm = new PixelMath;
      pm.expression = steps[i];
      pm.executeOn( m.mainView );
      pump( 1200 );
   }
   log.push( "launchInterface=" + ( new PICopilot ).launchInterface() );
   phase( "gui.panel", { moveTo: [ 1200, 120, 600, 720 ], out: DIR + "/panel.json" } );
   m.bringToFront();
   pump( 3000 );   // the journey ticks join and record the master
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
mark( "ready", log.join( "\n" ) );
