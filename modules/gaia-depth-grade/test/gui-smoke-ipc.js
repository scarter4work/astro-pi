// GaiaDepthGrade GUI smoke, IPC requests (test/gui-smoke.sh; run in the RUNNING PixInsight with
// -x=<slot>:<this file>). Test-only, never shipped.
//
// The running instance keeps its start-up environment (a -x launcher's variables never reach it),
// so the request is the file $GDG_GUI_DIR/ipc-request.json:
//   { "action": "windows", "out": <file> }            every image window, described, as JSON
//   { "action": "close", "id": <view id>, "out": <file> }   force-close that window, then the same
// The result is written to <out>.tmp and renamed, so the shell never reads half a file.
//
// ES5 only.

var DIR = getEnvironmentVariable( "GDG_GUI_DIR" );

function describe( w )
{
   var img = w.mainView.image, k = w.keywords, depthtag = null, history = [];
   for ( var i = 0; i < k.length; ++i )
   {
      var n = k[i].name.trim();
      if ( n == "DEPTHTAG" )
         depthtag = k[i].strippedValue;
      else if ( n == "HISTORY" )
         history.push( k[i].comment.trim() );
   }
   return { id: w.mainView.id, width: img.width, height: img.height, channels: img.numberOfChannels,
            bitsPerSample: img.bitsPerSample, isReal: img.isReal, visible: w.visible, depthtag: depthtag,
            history: history, hasSolution: w.hasAstrometricSolution };
}

var req = JSON.parse( File.readTextFile( DIR + "/ipc-request.json" ) );
var result = { ok: true, action: req.action };
try
{
   if ( req.action == "close" )
   {
      var w = ImageWindow.windowById( req.id );
      if ( w.isNull )
         throw new Error( "no window " + req.id );
      w.forceClose();
      processEvents();
      result.closed = req.id;
   }
   else if ( req.action != "windows" )
      throw new Error( "unknown action " + req.action );
   var ws = ImageWindow.windows, list = [];
   for ( var i = 0; i < ws.length; ++i )
      list.push( describe( ws[i] ) );
   result.windows = list;
}
catch ( e )
{
   result.ok = false;
   result.error = String( e );
}
File.writeTextFile( req.out + ".tmp", JSON.stringify( result ) );
File.move( req.out + ".tmp", req.out );
