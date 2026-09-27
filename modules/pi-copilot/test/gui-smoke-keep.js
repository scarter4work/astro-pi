// PI Copilot GUI smoke, step 3 (test/gui-smoke.sh; sent with -x): starts a
// keeper write-up against a loopback that accepts and never answers, closes
// the images (no "save changes?" prompt at Quit), and ends. The driver then
// quits PixInsight from the File menu with that write-up in flight.
#include "gui-smoke-common.jsh"

var log = [];
try
{
   var url = File.readTextFile( DIR + "/stall-url" ).trim();
   phase( "gui.keepInFlight", { viewId: "pcGuiM31", url: url, out: DIR + "/keep.json" } );
   var ws = ImageWindow.windows;
   for ( var i = 0; i < ws.length; ++i )
      ws[i].forceClose();
   log.push( "keep started" );
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
mark( "keep-end", String( Date.now() ) + "\n" + log.join( "\n" ) );
