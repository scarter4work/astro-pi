// EZTools EZHazeKill headless parity probe. Test-only, never shipped. ES5 only.
//
// For every case: open the input, run the EZHazeKill PROCESS (the -m= module) on the main view or
// on a preview, capture the console, save the result.
//
// Input:  $EZT_PROBE_IN  -- { "cases": [ { name, input, output, log, amount, neutralize, target,
//                             preview?: [x0,y0,x1,y1], expect_error? } ] }
// Output: $EZT_PROBE_OUT -- { "ok": bool, "core": "...", "cases": [ { name, ok, error?, log, seconds } ] }
// A case with inject_nan: [x, y, channel] gets a NaN written into that sample after opening
// (PixInsight's XISF reader turns a stored NaN into 0), and reports "unchanged": whether every
// sample after executeOn equals the one before (NaN-aware).

#include <pjsr/UndoFlag.jsh>

// Every sample of every channel of img, one Float64Array per channel.
function eztSamples( img )
{
   var out = [];
   for ( var c = 0; c < img.numberOfChannels; ++c )
   {
      var a = new Float64Array( img.width*img.height );
      img.getSamples( a, new Rect( 0, 0, img.width, img.height ), c );
      out.push( a );
   }
   return out;
}

function eztSameSamples( a, b )
{
   if ( a.length != b.length )
      return false;
   for ( var c = 0; c < a.length; ++c )
   {
      var x = a[c], y = b[c];
      if ( x.length != y.length )
         return false;
      for ( var i = 0; i < x.length; ++i )
         if ( x[i] !== y[i] && !( x[i] != x[i] && y[i] != y[i] ) )
            return false;
   }
   return true;
}

function eztSaveView( view, path )
{
   if ( view.isMainView )
   {
      if ( !view.window.saveAs( path, false, false, false, false ) )
         throw new Error( "saveAs " + path + " failed" );
      return;
   }
   // A preview: copy its pixels into a new window with the same sample format.
   var img = view.image;
   var w = new ImageWindow( img.width, img.height, img.numberOfChannels, img.bitsPerSample, img.isReal,
                            img.isColor, "ezt_preview_out" );
   try
   {
      w.mainView.beginProcess( UndoFlag_NoSwapFile );
      w.mainView.image.assign( img );
      w.mainView.endProcess();
      if ( !w.saveAs( path, false, false, false, false ) )
         throw new Error( "saveAs " + path + " failed" );
   }
   finally
   {
      w.forceClose();
   }
}

function eztProbeMain()
{
   var inPath = getEnvironmentVariable( "EZT_PROBE_IN" );
   var outPath = getEnvironmentVariable( "EZT_PROBE_OUT" );
   var result = { ok: true, core: format( "%d.%d.%d", coreVersionMajor, coreVersionMinor, coreVersionRelease ), cases: [] };
   try
   {
      var cfg = JSON.parse( File.readTextFile( inPath ) );
      for ( var i = 0; i < cfg.cases.length; ++i )
      {
         var c = cfg.cases[i];
         var r = { name: c.name, ok: false, log: c.log };
         result.cases.push( r );
         var w = null;
         try
         {
            var ws = ImageWindow.open( c.input );
            if ( ws.length != 1 )
               throw new Error( "opening " + c.input + " gave " + ws.length + " windows" );
            w = ws[0];
            var view = w.mainView;
            if ( c.preview )
               view = w.createPreview( new Rect( c.preview[0], c.preview[1], c.preview[2], c.preview[3] ), "ezt_preview" );

            var before = null;
            if ( c.inject_nan )
            {
               view.beginProcess( UndoFlag_NoSwapFile );
               view.image.setSample( NaN, c.inject_nan[0], c.inject_nan[1], c.inject_nan[2] );
               view.endProcess();
               if ( !isNaN( view.image.sample( c.inject_nan[0], c.inject_nan[1], c.inject_nan[2] ) ) )
                  throw new Error( "could not inject a NaN sample" );
               before = eztSamples( view.image );
            }

            var P = new EZHazeKill;
            P.amount = c.amount;
            P.neutralize = c.neutralize;
            P.targetLevel = c.target;

            var t0 = Date.now();
            console.beginLog();
            var executed = false;
            try
            {
               executed = P.executeOn( view );
               if ( !executed )
                  r.error = "executeOn returned false";
            }
            catch ( e )
            {
               r.error = e.toString();
            }
            var log = console.endLog();
            r.seconds = ( Date.now() - t0 )/1000;
            File.writeFile( c.log, log );
            if ( before != null )
               r.unchanged = eztSameSamples( before, eztSamples( view.image ) );
            eztSaveView( view, c.output );
            r.ok = executed;
            if ( !executed && !c.expect_error )
               result.ok = false;
         }
         catch ( e )
         {
            r.error = e.toString();
            result.ok = false;
         }
         if ( w != null )
            w.forceClose();
      }
   }
   catch ( e )
   {
      result.ok = false;
      result.error = e.toString();
   }
   File.writeTextFile( outPath, JSON.stringify( result, null, 1 ) );
}

eztProbeMain();
