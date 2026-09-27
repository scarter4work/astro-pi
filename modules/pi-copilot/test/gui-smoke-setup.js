// PI Copilot GUI smoke, step 1 (test/gui-smoke.sh; -r at start-up): two
// masters with History steps made at top level (recorded by the production
// JourneyService), then the PI Copilot panel shown next to them. The script
// ENDS so the GUI is idle (PixInsight disables the panel while a script runs)
// for the driver's clicks.
#include "gui-smoke-common.jsh"

function master( id, object, seed )
{
   var w = new ImageWindow( 320, 240, 1, 32, true, false, id );
   w.keywords = [ new FITSKeyword( "IMAGETYP", "'Master Light'", "" ),
                  new FITSKeyword( "OBJECT", "'" + object + "'", "" ),
                  new FITSKeyword( "FILTER", "'Ha'", "" ),
                  new FITSKeyword( "EXPTIME", "300", "" ),
                  new FITSKeyword( "NCOMBINE", "20", "" ) ];
   var p = new PixelMath;
   p.expression = "0.05 + 0.02*sin(x()/" + seed + ")*cos(y()/7) + 0.01*random()";
   p.executeOn( w.mainView );
   w.show();
   pump( 1500 );
   return w;
}

var log = [];
try
{
   var a = master( "pcGuiM42", "GuiM42", 11 );
   var steps = [ "$T*1.5", "$T+0.02", "mtf(0.3,$T)" ];
   for ( var i = 0; i < steps.length; ++i )
   {
      var pm = new PixelMath;
      pm.expression = steps[i];
      pm.executeOn( a.mainView );
      pump( 1200 );
   }
   var b = master( "pcGuiM31", "GuiM31", 5 );
   var pb = new PixelMath;
   pb.expression = "$T*1.2";
   pb.executeOn( b.mainView );
   pump( 1200 );
   console.hide();
   // launchInterface() from a script builds the panel but leaves it hidden
   // (measured); gui.panel shows it and reports where it is.
   log.push( "launchInterface=" + ( new PICopilot ).launchInterface() );
   phase( "gui.panel", { moveTo: [ 1200, 120, 600, 720 ], out: DIR + "/panel.json" } );
   a.bringToFront();
   pump( 2500 );
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
mark( "ready", log.join( "\n" ) );
