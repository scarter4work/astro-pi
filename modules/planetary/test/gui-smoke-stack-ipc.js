// Planetary GUI smoke for PlanetaryStack (test/gui-smoke-stack.sh; -x IPC into
// the running GUI). An IPC script sees the environment PixInsight started with,
// not the caller's, so each call's request is a file the shell writes first:
// $PLANETARY_GUI_DIR/ipc-request.json, { "mode": ..., "out": <file name> }.
//   windows  every image window: id, size, channels, its SRCVIDEO / NFRAMES /
//            NCOMBINE keywords and the samples asked for ("sample": [[x, y]],
//            every channel); then, with "close": true, each is closed (so the
//            smoke's later "no new window" checks and Quit start from none)
//   launch   the PlanetaryStack interface launched from an instance with
//            filePath = "path" (a saved process icon's way in: ImportProcess)
// The result goes to $PLANETARY_GUI_DIR/<out> as JSON. Test-only.
//
// ES5 only: an ES6 `class` anywhere kills a script before line 1 runs.

var DIR = getEnvironmentVariable( "PLANETARY_GUI_DIR" );
var result = {};
var out = "ipc-error.json";
try
{
   var req = JSON.parse( File.readTextFile( DIR + "/ipc-request.json" ) );
   out = req.out;
   if ( req.mode == "windows" )
   {
      var list = [];
      var windows = ImageWindow.windows;
      for ( var i = 0; i < windows.length; ++i )
      {
         var w = windows[i];
         var img = w.mainView.image;
         var entry = { id: w.mainView.id, width: img.width, height: img.height,
                       channels: img.numberOfChannels, keywords: {}, samples: [] };
         var kw = w.keywords;
         for ( var k = 0; k < kw.length; ++k )
            if ( kw[k].name == "SRCVIDEO" || kw[k].name == "NFRAMES" || kw[k].name == "NCOMBINE" )
               entry.keywords[kw[k].name] = kw[k].strippedValue;
         var samples = req.sample || [];
         for ( var s = 0; s < samples.length; ++s )
         {
            var v = [];
            for ( var c = 0; c < img.numberOfChannels; ++c )
               v.push( img.sample( samples[s][0], samples[s][1], c ) );
            entry.samples.push( v );
         }
         list.push( entry );
      }
      result.windows = list;
      if ( req.close )
         for ( var j = 0; j < windows.length; ++j )
            windows[j].forceClose();
   }
   else if ( req.mode == "launch" )
   {
      var P = new PlanetaryStack;
      P.filePath = req.path;
      result.filePath = P.filePath;
      result.launchInterface = P.launchInterface();
   }
   else
      throw new Error( "unknown mode '" + req.mode + "'" );
}
catch ( e )
{
   result.error = String( e );
}
File.writeTextFile( DIR + "/" + out, JSON.stringify( result ) );
