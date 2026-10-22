// Planetary headless probe: runs PlanetaryStack.executeGlobal() on each case
// and checks the window it opens, or that it fails loudly and leaves nothing.
// Test-only, never shipped.
//
// Input:  $PLANETARY_PROBE_IN -- written by run-probe-stack.sh from
//   make-fixtures --stack and the engine's own stack of each fixture:
//   { "cases": [
//     { kind: "image", name, path, bayerPattern?, expectId, width, height, channels,
//       samples: [ {x, y, c, v} ]            (the engine's values, tolerance TOLERANCE),
//       keywords: [ {name, value | comment | commentContains} ], absentKeywords: [ name ],
//       absentComments: [ text ] (no keyword comment contains it),
//       consoleContains: [ text ] },
//     { kind: "diagnosis", name, path, bayerPattern?, expectError },   (the engine's message)
//     { kind: "error", name, path, expectError },
//     { kind: "cancel", name, path, abortAfter, after: <an image case> } ] }
// Output: $PLANETARY_PROBE_OUT -- { "ok": bool, "cases": [ {name, ok, errors[], note?} ] }
//
// A diagnosis or error case must fail with the expected text (failedWith: no
// window, and for a diagnosis the message on the console too). A cancel case
// aborts the run from the Process Console (console.abort, as the console's
// Abort button does) abortAfter seconds in: it must fail saying it was
// cancelled, leave no window, leave no thread behind
// (/proc/self/status, against the count before the run) and the next run of
// the `after` case must succeed.
//
// ES5 only: an ES6 `class` anywhere kills a -r= script before line 1 runs.

#include "probe-common.jsh"

var PROBE_IN = getEnvironmentVariable( "PLANETARY_PROBE_IN" );
var PROBE_OUT = getEnvironmentVariable( "PLANETARY_PROBE_OUT" );
// The engine's sums change with the thread count at float rounding (1 vs 8
// threads agree within 1e-6, test_engine); PixInsight and stack-probe may use
// different counts.
var TOLERANCE = 1e-5;

function threadCount()
{
   var m = File.readTextFile( "/proc/self/status" ).match( /Threads:\s*(\d+)/ );
   if ( m === null )
      throw new Error( "no Threads line in /proc/self/status" );
   return parseInt( m[1] );
}

function newProcess( c )
{
   var P = new PlanetaryStack;
   P.filePath = c.path;
   if ( c.bayerPattern !== undefined )
   {
      var element = PlanetaryStack.prototype[c.bayerPattern];
      if ( element === undefined )
         throw new Error( "PlanetaryStack has no enum element " + c.bayerPattern );
      P.bayerPattern = element;
   }
   return P;
}

// Runs executeGlobal with the console captured: { ok, threw, message, log }.
function execute( P )
{
   var out = { ok: undefined, threw: false, message: "", log: "" };
   Console.beginLog();
   try
   {
      out.ok = P.executeGlobal();
   }
   catch ( e )
   {
      out.threw = true;
      out.message = String( e );
   }
   out.log = Console.endLog().toString();
   return out;
}

function runImageCase( c, r )
{
   var before = windowIds().length;
   var x = execute( newProcess( c ) );
   if ( x.threw || x.ok !== true )
   {
      r.errors.push( "executeGlobal " + (x.threw ? "threw '" + x.message + "'" : "returned " + x.ok) + "; console: " +
                     x.log.substring( Math.max( 0, x.log.length - 1500 ) ) );
      return;
   }
   if ( windowIds().length != before + 1 )
      r.errors.push( "expected one new window, have: " + windowIds().join( "," ) );
   var w = ImageWindow.activeWindow;
   if ( w.isNull )
   {
      r.errors.push( "no active image window after executeGlobal" );
      return;
   }
   var v = w.mainView;
   var img = v.image;
   if ( v.id != c.expectId )
      r.errors.push( "window id " + v.id + ", expected " + c.expectId );
   if ( img.width != c.width || img.height != c.height || img.numberOfChannels != c.channels )
      r.errors.push( "geometry " + img.width + "x" + img.height + "x" + img.numberOfChannels +
                     ", expected " + c.width + "x" + c.height + "x" + c.channels );
   else
   {
      if ( img.bitsPerSample != 32 || !img.isReal )
         r.errors.push( "sample format " + img.bitsPerSample + "-bit real=" + img.isReal + ", expected 32-bit float" );
      if ( (c.channels == 3) != img.isColor )
         r.errors.push( "isColor " + img.isColor + " for " + c.channels + " channels" );
      if ( c.samples.length == 0 )
         r.errors.push( "no engine samples to compare" );
      for ( var i = 0; i < c.samples.length; ++i )
      {
         var s = c.samples[i];
         var got = img.sample( s.x, s.y, s.c );
         if ( Math.abs( got - s.v ) > TOLERANCE )
            r.errors.push( "sample(" + s.x + "," + s.y + "," + s.c + ") = " + got + ", engine " + s.v );
      }
   }
   var kws = w.keywords;
   checkKeywords( kws, c.keywords, c.absentKeywords, r.errors );
   checkAbsentComments( kws, c.absentComments, r.errors );
   for ( var t = 0; t < c.consoleContains.length; ++t )
      if ( x.log.indexOf( c.consoleContains[t] ) < 0 )
         r.errors.push( "console lacks '" + c.consoleContains[t] + "': " + x.log.substring( 0, 1500 ) );
   w.forceClose();
}

// A failure is loud either way PixInsight reports it: executeGlobal throws
// (a CanExecuteGlobal refusal), or it returns false with the error that
// ExecuteGlobal threw on the Process Console. r.note records which.
function failedWith( x, text, r )
{
   if ( x.threw )
   {
      r.note = "exception";
      if ( x.message.indexOf( text ) < 0 )
         r.errors.push( "threw '" + x.message + "', expected text '" + text + "'" );
   }
   else if ( x.ok === false )
   {
      r.note = "console";
      if ( x.log.indexOf( text ) < 0 )
         r.errors.push( "returned false; console lacks '" + text + "': " + x.log.substring( 0, 900 ) );
   }
   else
      r.errors.push( "expected a failure containing '" + text + "', but executeGlobal returned " + x.ok );
}

function runFailureCase( c, r )
{
   var before = windowIds().length;
   var x = execute( newProcess( c ) );
   failedWith( x, c.expectError, r );
   if ( c.kind == "diagnosis" && x.log.indexOf( c.expectError ) < 0 )
      r.errors.push( "console lacks the diagnosis '" + c.expectError + "': " + x.log.substring( 0, 900 ) );
   if ( windowIds().length != before )
      r.errors.push( "a failed run left a new image window open: " + windowIds().join( "," ) );
}

function runCancelCase( c, r )
{
   var before = windowIds().length;
   msleep( 500 );
   var threadsBefore = threadCount();
   var fired = false;
   var timer = new Timer;
   timer.interval = c.abortAfter;
   timer.singleShot = true;
   timer.onTimeout = function()
   {
      fired = true;
      console.abort( true/*dontAsk*/ );
   };
   timer.start();
   var t0 = Date.now();
   var x = execute( newProcess( c ) );
   var seconds = (Date.now() - t0) / 1000;
   timer.stop();
   if ( !fired )
      r.errors.push( "the abort timer never fired (the run did not process events)" );
   failedWith( x, "PlanetaryStack: cancelled; no image was made", r );
   var how = r.note;
   if ( windowIds().length != before )
      r.errors.push( "a cancelled run left a window open: " + windowIds().join( "," ) );
   // Joined threads leave /proc/self/task once the kernel reaps them.
   var threadsAfter = threadCount();
   for ( var k = 0; k < 50 && threadsAfter > threadsBefore; ++k )
   {
      msleep( 100 );
      threadsAfter = threadCount();
   }
   if ( threadsAfter > threadsBefore )
      r.errors.push( threadsAfter + " threads after the cancel, " + threadsBefore + " before" );
   // The phase the abort landed in: the last progress line the run started.
   var phases = x.log.match( /PlanetaryStack: (Measuring frames|Aligning|Stacking):/g );
   if ( phases === null )
      r.errors.push( "the run was aborted before any progress (not mid-run): " + x.log.substring( 0, 900 ) );
   var phase = phases === null ? "none" : phases[phases.length - 1].replace( /^PlanetaryStack: |:$/g, "" );
   r.note = how + "; aborted in " + phase + " after " + seconds.toFixed( 2 ) + " s; threads " + threadsBefore + " -> " +
            threadsAfter;

   // PixInsight is still usable: the next stack succeeds. An accepted abort
   // stands for the rest of the running script -- measured: a built-in
   // Convolution run after it fails at once with "Process aborted" too, until
   // console.resetStatus() -- while a run started anew from the GUI begins
   // clean; resetStatus() is that new start.
   console.resetStatus();
   var after = { name: c.after.name, errors: [] };
   runImageCase( c.after, after );
   for ( var e = 0; e < after.errors.length; ++e )
      r.errors.push( "run after the cancel: " + after.errors[e] );
}

function main()
{
   var result = { ok: false, cases: [] };
   try
   {
      var input = JSON.parse( File.readTextFile( PROBE_IN ) );
      for ( var i = 0; i < input.cases.length; ++i )
      {
         var c = input.cases[i];
         var r = { name: c.name, ok: false, errors: [] };
         try
         {
            closeAllWindows();
            if ( c.kind == "image" )
               runImageCase( c, r );
            else if ( c.kind == "diagnosis" || c.kind == "error" )
               runFailureCase( c, r );
            else if ( c.kind == "cancel" )
               runCancelCase( c, r );
            else
               r.errors.push( "unknown case kind " + c.kind );
         }
         catch ( e )
         {
            r.errors.push( "exception: " + e );
         }
         r.ok = r.errors.length == 0;
         result.cases.push( r );
      }
      var allOk = result.cases.length == input.cases.length && result.cases.length > 0;
      for ( var n = 0; n < result.cases.length; ++n )
         allOk = allOk && result.cases[n].ok;
      result.ok = allOk;
   }
   catch ( e )
   {
      result.error = "probe failed: " + e;
   }
   File.writeTextFile( PROBE_OUT, JSON.stringify( result, null, 1 ) );
}

main();
