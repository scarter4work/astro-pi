// PI Copilot quit-mid-tick test (test/quit-mid-tick.sh), step 1 (-r at start-up):
// masters with History steps, recorded by the production JourneyService, so the
// journey ticks have real images to scan. The script ENDS so the GUI is idle.
#include "gui-smoke-common.jsh"

function master( id, object, seed )
{
   var w = new ImageWindow( 640, 480, 1, 32, true, false, id );
   w.keywords = [ new FITSKeyword( "IMAGETYP", "'Master Light'", "" ),
                  new FITSKeyword( "OBJECT", "'" + object + "'", "" ),
                  new FITSKeyword( "FILTER", "'Ha'", "" ),
                  new FITSKeyword( "EXPTIME", "300", "" ),
                  new FITSKeyword( "NCOMBINE", "20", "" ) ];
   var p = new PixelMath;
   p.expression = "0.05 + 0.02*sin(x()/" + seed + ")*cos(y()/7) + 0.01*random()";
   p.executeOn( w.mainView );
   w.show();
   return w;
}

var log = [];
try
{
   var steps = [ "$T*1.1", "$T+0.01", "mtf(0.4,$T)", "$T*0.95" ];
   for ( var k = 0; k < 4; ++k )
   {
      var w = master( "pcQuitM" + k, "QuitObj" + k, 3 + k );
      for ( var i = 0; i < steps.length; ++i )
      {
         var pm = new PixelMath;
         pm.expression = steps[i];
         pm.executeOn( w.mainView );
      }
      pump( 600 );
   }
   pump( 3000 );   // the journey ticks join and record them
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
mark( "ready", log.join( "\n" ) );
