// GaiaDepthGrade process probe (plan Task 16). Test-only, never shipped.
//
// Runs the GaiaDepthGrade process the way a process icon, a script or a batch runs it
// (new GaiaDepthGrade; executeOn), on a master opened here, and writes what happened to
// $GDG_PROBE_OUT as JSON. The request is the JSON file $GDG_PROC_REQ:
//   master     image file to open (the target)
//   mono       true: the target is a one-channel copy (the channel mean) of the master
//   params     GaiaDepthGrade parameters to set ({ id: value }; gaiaDatabaseFiles: [paths])
//   split      true: first split the target with StarXTerminator here (output_stars, unscreen),
//              and pass the two layers as starsViewId / starlessViewId (splitMode SuppliedViews)
//   saveTo     after a successful run in ReplaceTarget mode: save the target to this file
//   compareTo  an earlier result's output file: report max |difference| to it
//   saveOutput save the output (the new window, or the replaced target) to this file
//   extraMono  also make a one-channel copy of the target with this id (a wrong-size layer)
//   repeat     run the instance a second time: its output vs the first (determinism)
//   thenReplace run it again with outputMode ReplaceTarget: the target vs the first output
//   sxtNoise   run StarXTerminator twice on the target here: its own run-to-run max |d|
//   thenFail   run the same instance once more so that it fails: its QA outputs afterwards
// The module's test hooks write the run's last error ($GDG_TEST_LAST_ERROR) and its engine
// settings ($GDG_TEST_DUMP_SETTINGS); this script reads them back.
//
// ES5 only: an ES6 `class` anywhere kills a -r= script before line 1 runs.

#include <pjsr/UndoFlag.jsh>

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );
var REQ = getEnvironmentVariable( "GDG_PROC_REQ" );
var ERRF = getEnvironmentVariable( "GDG_TEST_LAST_ERROR" );
var DUMPF = getEnvironmentVariable( "GDG_TEST_DUMP_SETTINGS" );

function readText( path )
{
   return File.exists( path ) ? File.readTextFile( path ) : "";
}

function keywordsOf( w )
{
   var k = w.keywords, r = { depthtag: null, history: [], count: k.length };
   for ( var i = 0; i < k.length; ++i )
   {
      var n = k[i].name.trim();
      if ( n == "DEPTHTAG" )
         r.depthtag = k[i].strippedValue;
      else if ( n == "HISTORY" )
         r.history.push( k[i].comment.trim() );
   }
   return r;
}

function describe( w )
{
   var img = w.mainView.image;
   return { id: w.mainView.id, width: img.width, height: img.height, channels: img.numberOfChannels,
            bitsPerSample: img.bitsPerSample, isReal: img.isReal, keywords: keywordsOf( w ),
            hasSolution: w.hasAstrometricSolution, min: img.minimum(), max: img.maximum(), mean: img.mean() };
}

function windowIds()
{
   var ids = [], ws = ImageWindow.windows;
   for ( var i = 0; i < ws.length; ++i )
      ids.push( ws[i].mainView.id );
   return ids;
}

// A hidden copy of an image (pixels only), for exact before/after comparison.
function copyOf( img, id )
{
   var w = new ImageWindow( img.width, img.height, img.numberOfChannels, img.bitsPerSample, img.isReal, img.isColor, id );
   w.mainView.beginProcess( UndoFlag_NoSwapFile );
   w.mainView.image.assign( img );
   w.mainView.endProcess();
   return w;
}

// max |a - b| over every sample (same geometry required).
function maxAbsDiff( a, b )
{
   if ( a.width != b.width || a.height != b.height || a.numberOfChannels != b.numberOfChannels )
      return -1;
   var d = 0, n = a.width * a.height, va = new Float64Array( n ), vb = new Float64Array( n );
   for ( var c = 0; c < a.numberOfChannels; ++c )
   {
      a.getSamples( va, new Rect( a.width, a.height ), c );
      b.getSamples( vb, new Rect( b.width, b.height ), c );
      for ( var i = 0; i < n; ++i )
      {
         var x = Math.abs( va[i] - vb[i] );
         if ( x > d )
            d = x;
      }
   }
   return d;
}

function monoCopy( w, id )
{
   var img = w.mainView.image, n = img.width * img.height;
   var m = new ImageWindow( img.width, img.height, 1, 32, true, false, id );
   var acc = new Float64Array( n ), v = new Float64Array( n );
   for ( var c = 0; c < img.numberOfChannels; ++c )
   {
      img.getSamples( v, new Rect( img.width, img.height ), c );
      for ( var i = 0; i < n; ++i )
         acc[i] += v[i] / img.numberOfChannels;
   }
   m.mainView.beginProcess( UndoFlag_NoSwapFile );
   m.mainView.image.setSamples( acc, new Rect( img.width, img.height ), 0 );
   m.mainView.endProcess();
   m.keywords = w.keywords;
   return m;
}

var result = { ok: true };
try
{
   var A = JSON.parse( File.readTextFile( REQ ) );
   if ( ERRF && File.exists( ERRF ) )
      File.remove( ERRF );
   if ( DUMPF && File.exists( DUMPF ) )
      File.remove( DUMPF );

   var opened = ImageWindow.open( A.master );
   if ( !opened || opened.length == 0 )
      throw new Error( "cannot open " + A.master );
   var target = opened[0];
   if ( A.mono )
   {
      var m = monoCopy( target, "gdgmono" );
      target.forceClose();
      target = m;
   }
   target.show();
   result.target = describe( target );
   if ( A.extraMono )
      monoCopy( target, A.extraMono );

   if ( A.split )
   {
      var before = {}, ws = ImageWindow.windows;
      for ( var i = 0; i < ws.length; ++i )
         before[ws[i].mainView.id] = true;
      var starless = copyOf( target.mainView.image, "probe_starless" );
      var sxt = new StarXTerminator;
      sxt.output_stars = true;
      sxt.unscreen = true;
      if ( !sxt.executeOn( starless.mainView ) )
         throw new Error( "StarXTerminator failed in the probe" );
      ws = ImageWindow.windows;
      var stars = null;
      for ( var j = 0; j < ws.length; ++j )
         if ( !before[ws[j].mainView.id] && ws[j].mainView.id != "probe_starless" )
            stars = ws[j];
      if ( stars == null )
         throw new Error( "StarXTerminator made no stars window in the probe" );
      A.params.splitMode = 1; // SuppliedViews
      A.params.starsViewId = stars.mainView.id;
      A.params.starlessViewId = starless.mainView.id;
      result.supplied = { stars: stars.mainView.id, starless: starless.mainView.id };
   }

   var reference = copyOf( target.mainView.image, "probe_reference" );
   var keywordsBefore = target.keywords.length;
   var windowsBefore = windowIds();

   var P = new GaiaDepthGrade;
   for ( var key in A.params )
   {
      if ( key == "gaiaDatabaseFiles" )
      {
         var rows = [];
         for ( var r = 0; r < A.params[key].length; ++r )
            rows.push( [ A.params[key][r] ] );
         P.gaiaDatabaseFiles = rows;
      }
      else
         P[key] = A.params[key];
   }
   var t0 = Date.now();
   // A CanExecuteOn refusal throws here (the core's "Cannot execute instance on view ... Reason: ..."),
   // where a failed ExecuteOn returns false: both are recorded as not executed.
   result.executeThrew = "";
   try
   {
      result.executed = P.executeOn( target.mainView );
   }
   catch ( e )
   {
      result.executed = false;
      result.executeThrew = String( e );
   }
   result.seconds = (Date.now() - t0) / 1000;
   result.error = ERRF ? readText( ERRF ) : "";
   result.settings = DUMPF && File.exists( DUMPF ) ? JSON.parse( readText( DUMPF ) ) : null;
   result.qa = { nDetected: P.outNDetected, nMatched: P.outNMatched, matchRate: P.outMatchRate,
                 medianOffset: P.outMedianOffset, lowMatch: P.outLowMatch };

   // Windows the run left: everything new except our own helpers.
   var after = windowIds(), made = [];
   for ( var a = 0; a < after.length; ++a )
      if ( windowsBefore.indexOf( after[a] ) < 0 )
         made.push( describe( ImageWindow.windowById( after[a] ) ) );
   result.newWindows = made;
   result.windowsAfter = after;

   // The target: unchanged (NewWindow) or graded in place (ReplaceTarget).
   result.targetMaxDiff = maxAbsDiff( reference.mainView.image, target.mainView.image );
   result.targetAfter = describe( target );
   result.targetKeywordsBefore = keywordsBefore;
   result.targetModified = target.isModified;

   var output = made.length == 1 ? ImageWindow.windowById( made[0].id ) : (result.targetMaxDiff > 0 ? target : null);
   if ( output && A.saveOutput )
      result.savedOutput = output.saveAs( A.saveOutput, false, false, false, false );
   if ( output && A.compareTo )
   {
      var cmp = ImageWindow.open( A.compareTo )[0];
      result.compareMaxDiff = maxAbsDiff( cmp.mainView.image, output.mainView.image );
      cmp.forceClose();
   }
   if ( A.saveTo && result.executed && !A.thenReplace )
      result.savedTo = target.saveAs( A.saveTo, false, false, false, false ) && File.exists( A.saveTo );

   var first = made.length == 1 ? ImageWindow.windowById( made[0].id ) : null;
   // repeat: the same instance once more on the same target (and layers): output vs the first.
   if ( A.repeat && result.executed && first )
   {
      var ids1 = windowIds();
      result.repeatExecuted = P.executeOn( target.mainView );
      result.repeatQA = { nDetected: P.outNDetected, nMatched: P.outNMatched, matchRate: P.outMatchRate,
                          medianOffset: P.outMedianOffset, lowMatch: P.outLowMatch };
      var ids2 = windowIds(), second = null;
      for ( var k = 0; k < ids2.length; ++k )
         if ( ids1.indexOf( ids2[k] ) < 0 )
            second = ImageWindow.windowById( ids2[k] );
      result.repeatMaxDiff = second ? maxAbsDiff( first.mainView.image, second.mainView.image ) : -1;
      result.repeatId = second ? second.mainView.id : null;
      if ( second )
         second.forceClose();
   }
   // thenReplace: outputMode ReplaceTarget on the same target: the target vs the new-window output.
   if ( A.thenReplace && result.executed && first )
   {
      var idsR = windowIds();
      P.outputMode = 1; // ReplaceTarget
      result.replaceExecuted = P.executeOn( target.mainView );
      result.replaceError = ERRF ? readText( ERRF ) : "";
      result.replaceNewWindows = windowIds().length - idsR.length;
      result.replaceMaxDiff = maxAbsDiff( first.mainView.image, target.mainView.image );
      result.replaceTargetAfter = describe( target );
      if ( A.saveTo && result.replaceExecuted )
         result.savedTo = target.saveAs( A.saveTo, false, false, false, false ) && File.exists( A.saveTo );
      if ( A.saveTo && result.savedTo )
      {
         var back = ImageWindow.open( A.saveTo )[0];
         result.savedMaxDiff = maxAbsDiff( back.mainView.image, first.mainView.image );
         result.savedKeywords = keywordsOf( back );
         back.forceClose();
      }
   }
   // sxtNoise: StarXTerminator twice on copies of the target: the max |d| of its two stars layers
   // (its own run-to-run noise: the bound for comparing two runs that each split again).
   if ( A.sxtNoise )
   {
      var na = copyOf( reference.mainView.image, "noise_a" ), nb = copyOf( reference.mainView.image, "noise_b" );
      var s2 = new StarXTerminator;
      s2.output_stars = true;
      s2.unscreen = true;
      s2.executeOn( na.mainView );
      s2.executeOn( nb.mainView );
      result.sxtNoise = maxAbsDiff( ImageWindow.windowById( "noise_a_stars" ).mainView.image,
                                    ImageWindow.windowById( "noise_b_stars" ).mainView.image );
   }
   // thenFail: the same instance (QA of a successful run in its read-only outputs) executed again
   // so that it fails (supplied views that do not exist): the QA outputs must not keep the earlier
   // run's values.
   if ( A.thenFail && result.executed )
   {
      P.outputMode = 0;
      P.splitMode = 1; // SuppliedViews
      P.starsViewId = "gdg_no_such_view";
      P.starlessViewId = "gdg_no_such_view";
      result.failExecuted = P.executeOn( target.mainView );
      result.failError = ERRF ? readText( ERRF ) : "";
      result.failQA = { nDetected: P.outNDetected, nMatched: P.outNMatched, matchRate: P.outMatchRate,
                        medianOffset: P.outMedianOffset, lowMatch: P.outLowMatch };
   }
   reference.forceClose();
}
catch ( e )
{
   result = { ok: false, error: "probe-process.js: " + e.toString() };
}
File.writeTextFile( OUT, JSON.stringify( result ) + "\n" );
