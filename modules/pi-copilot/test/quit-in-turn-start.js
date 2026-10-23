// PI Copilot quit-in-turn test (test/quit-in-turn.sh), -x: arms ★ Keep's confirm stand-in (6 s of event
// pumping, answered Yes; gui.keepArm) and, in "tool" mode, starts a chat turn against the loopback stub,
// whose first reply asks for mark_journey_best -- the same keep flow, inside the tool loop. Mode and URL
// come from files in DIR.
#include "gui-smoke-common.jsh"

var log = [];
try
{
   var mode = File.readTextFile( DIR + "/mode" ).trim();
   phase( "gui.keepArm", { seconds: 6, standIn: mode != "keepmodal", out: DIR + "/keep.json" } );
   if ( mode == "tool" )
   {
      var url = File.readTextFile( DIR + "/stub-url" ).trim();
      phase( "gui.turn", { url: url, key: "sk-test-loopback", prompt: "Keep this journey.", out: DIR + "/turn.json" } );
   }
   log.push( "armed " + mode );
}
catch ( e )
{
   log.push( "ERROR " + String( e ) );
}
mark( "start-end", log.join( "\n" ) );
