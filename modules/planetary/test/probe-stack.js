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
//     { kind: "diagnosis", name, path, bayerPattern?, expectError,      (the engine's message)
//       numbers: [ {name, value} ] },                                   (its numbers, on the console too)
//     { kind: "error", name, path, expectError, consoleAbsent? },
//   Any case may set target (an enum element id, e.g. "TargetJupiter") and mineral
//   ({ strength?, chromaBlur? }: mineralMoon on). An image case with mineral also
//   has expectMineralId, mineralKeywords, mineralSamples and mineralHistory (the
//   engine's, stack-probe --mineral): its second window, checkMineralWindow.
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
   if ( c.target !== undefined )
   {
      var target = PlanetaryStack.prototype[c.target];
      if ( target === undefined )
         throw new Error( "PlanetaryStack has no enum element " + c.target );
      P.target = target;
   }
   if ( c.mineral !== undefined )
   {
      P.mineralMoon = true;
      if ( c.mineral.strength !== undefined )
         P.mineralStrength = c.mineral.strength;
      if ( c.mineral.chromaBlur !== undefined )
         P.chromaBlur = c.mineral.chromaBlur;
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

// The mineral moon's samples are compared looser than the stack's: its
// display transfer and sharpening magnify the stack's float rounding a little
// (measured: within 1e-6 over 1, 7 and 32 threads).
var MINERAL_TOLERANCE = 1e-4;

// The HISTORY cards of a window joined into one text (cards are split at
// spaces; joining them with single spaces restores each line).
function historyText( kws )
{
   var parts = [];
   for ( var i = 0; i < kws.length; ++i )
      if ( kws[i].name == "HISTORY" )
         parts.push( kws[i].comment.trim() );
   return parts.join( " " );
}

// The mineral moon's window (Task B8): the stack's size, RGB 32-bit float, every
// sample within [0, 1], the engine's mineral samples, its keywords, and the
// engine's seven step lines in HISTORY, in order, each once, after every line of the stack's.
function checkMineralWindow( c, stackImage, r )
{
   var mw = ImageWindow.windowById( c.expectMineralId );
   if ( mw.isNull )
   {
      r.errors.push( "no mineral moon window " + c.expectMineralId + "; have: " + windowIds().join( "," ) );
      return;
   }
   var img = mw.mainView.image;
   if ( img.width != stackImage.width || img.height != stackImage.height || img.numberOfChannels != 3 || !img.isColor )
      r.errors.push( "mineral moon geometry " + img.width + "x" + img.height + "x" + img.numberOfChannels +
                     ", expected the stack's " + stackImage.width + "x" + stackImage.height + "x3" );
   else
   {
      if ( img.bitsPerSample != 32 || !img.isReal )
         r.errors.push( "mineral moon sample format " + img.bitsPerSample + "-bit real=" + img.isReal );
      for ( var ch = 0; ch < 3; ++ch )
      {
         img.selectedChannel = ch;
         if ( img.minimum() < 0 || img.maximum() > 1 )
            r.errors.push( "mineral moon channel " + ch + " has samples outside [0, 1]: " + img.minimum() + " .. " +
                           img.maximum() );
      }
      img.resetSelections();
      if ( c.mineralSamples.length == 0 )
         r.errors.push( "no engine mineral samples to compare" );
      for ( var i = 0; i < c.mineralSamples.length; ++i )
      {
         var s = c.mineralSamples[i];
         var got = img.sample( s.x, s.y, s.c );
         if ( Math.abs( got - s.v ) > MINERAL_TOLERANCE )
            r.errors.push( "mineral sample(" + s.x + "," + s.y + "," + s.c + ") = " + got + ", engine " + s.v );
      }
   }
   var kws = mw.keywords;
   checkKeywords( kws, c.mineralKeywords, [], r.errors );
   var text = historyText( kws );
   if ( c.mineralHistory.length != 7 )
      r.errors.push( "the engine gave " + c.mineralHistory.length + " mineral HISTORY lines, expected 7" );
   // The stack's own lines end with its counts line ("... frames measured, ...") and any warnings: its last
   // "PlanetaryStack: " or "PlanetaryStack warning: " line. Every mineral line comes after that one (B8 review M3).
   var lastStack = Math.max( text.lastIndexOf( "PlanetaryStack: " ), text.lastIndexOf( "PlanetaryStack warning: " ) );
   if ( lastStack < 0 || text.indexOf( " frames measured, " ) < 0 )
      r.errors.push( "the mineral moon window lacks the stack's HISTORY lines: " + text );
   var firstMineral = text.indexOf( "PlanetaryStack mineral: " );
   if ( firstMineral >= 0 && firstMineral < lastStack )
      r.errors.push( "a mineral HISTORY line comes before the stack's last line: " + text );
   var pos = 0;
   for ( var k = 0; k < c.mineralHistory.length; ++k )
   {
      var line = "PlanetaryStack mineral: " + c.mineralHistory[k];
      var at = text.indexOf( line, pos );
      if ( at < 0 )
      {
         r.errors.push( "mineral HISTORY lacks, in order, '" + line + "': " + text );
         break;
      }
      pos = at + line.length;
   }
   var count = text.split( "PlanetaryStack mineral: " ).length - 1;
   if ( count != 7 )
      r.errors.push( count + " 'PlanetaryStack mineral: ' lines in HISTORY, expected 7" );
   mw.forceClose();
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
   var wanted = c.expectMineralId !== undefined ? 2 : 1;
   if ( windowIds().length != before + wanted )
      r.errors.push( "expected " + wanted + " new window(s), have: " + windowIds().join( "," ) );
   var w = ImageWindow.windowById( c.expectId );
   if ( w.isNull )
   {
      r.errors.push( "no image window " + c.expectId + " after executeGlobal; have: " + windowIds().join( "," ) );
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
   var absent = c.consoleAbsent || [];
   for ( var u = 0; u < absent.length; ++u )
      if ( x.log.indexOf( absent[u] ) >= 0 )
         r.errors.push( "console carries '" + absent[u] + "': " + x.log.substring( 0, 1500 ) );
   if ( c.expectMineralId !== undefined )
      checkMineralWindow( c, img, r );
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

// The diagnosis numbers follow its message on the console, in the engine's
// order, as "(<name> <value>, ...)" (%.4g; stack-probe's JSON has 4 decimals;
// null = not finite).
function checkNumbers( log, from, numbers, errors )
{
   if ( numbers.length == 0 )
      errors.push( "the engine gave this diagnosis no numbers" );
   var line = log.substring( from, log.indexOf( "\n", from ) < 0 ? log.length : log.indexOf( "\n", from ) );
   var pos = 0;
   for ( var i = 0; i < numbers.length; ++i )
   {
      var n = numbers[i];
      var at = line.indexOf( n.name + " ", pos );
      if ( at < 0 )
      {
         errors.push( "console lacks the diagnosis number '" + n.name + "' after the message: " + line );
         return;
      }
      pos = at + n.name.length + 1;
      var m = line.substring( pos ).match( /^(-?[0-9][0-9.e+-]*|-?inf|nan)/ );
      if ( m === null )
         errors.push( "no value after '" + n.name + "': " + line );
      else if ( n.value === null ? !/inf|nan/.test( m[1] )
                                 : Math.abs( parseFloat( m[1] ) - n.value ) > Math.max( 1e-3, 1e-3 * Math.abs( n.value ) ) )
         errors.push( "diagnosis number '" + n.name + "' reads " + m[1] + " on the console, engine " + n.value );
   }
}

function runFailureCase( c, r )
{
   var before = windowIds().length;
   var x = execute( newProcess( c ) );
   failedWith( x, c.expectError, r );
   if ( c.kind == "diagnosis" )
   {
      var at = x.log.indexOf( c.expectError );
      if ( at < 0 )
         r.errors.push( "console lacks the diagnosis '" + c.expectError + "': " + x.log.substring( 0, 900 ) );
      else
         checkNumbers( x.log, at + c.expectError.length, c.numbers, r.errors );
   }
   if ( windowIds().length != before )
      r.errors.push( "a failed run left a new image window open: " + windowIds().join( "," ) );
   // A refusal comes before any stacking (Mineral moon's: no progress line on the console).
   var absent = c.consoleAbsent || [];
   for ( var u = 0; u < absent.length; ++u )
      if ( x.log.indexOf( absent[u] ) >= 0 )
         r.errors.push( "console carries '" + absent[u] + "': " + x.log.substring( 0, 1500 ) );
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
