// Planetary headless probe: runs SERViewer.executeGlobal() on each fixture
// case and checks the window it opens. Test-only, never shipped.
//
// Input:  $PLANETARY_PROBE_IN  -- JSON written by make-fixtures:
//   { "cases": [ { name, path, frame, debayer,
//                  expectId, width, height, channels,
//                  samples: [ {x, y, c, v} ],                 (tolerance 1e-6)
//                  keywords: [ {name, value | comment | commentContains} ],
//                  absentKeywords: [ name ],
//                  absentComments: [ text ] (optional: no keyword comment contains it) }
//              | { name, path, frame, debayer, expectError }
//              | { name, path, frame, debayer, saveSerPath,
//                  expectConsole: [ text ] (all present, in this order),
//                  consoleAbsent: [ text ] } ] }
//   Any case may carry bayerPattern (a SERViewer enum element id, e.g.
//   "BayerRGGB") and saveSerPath. A case with saveSerPath and no expectError
//   is a conversion: executeGlobal must return true, open no window, write the
//   file and say so on the console.
// Output: $PLANETARY_PROBE_OUT -- { "ok": bool, "cases": [ {name, ok, errors[]} ] }
//
// An error case must fail loudly with the expected text: either executeGlobal
// throws it, or it returns false and the Process Console carries it (the
// output records which), and in both cases no image window may be left open.
//
// ES5 only: an ES6 `class` anywhere kills a -r= script before line 1 runs.

#include "probe-common.jsh"

var PROBE_IN = getEnvironmentVariable( "PLANETARY_PROBE_IN" );
var PROBE_OUT = getEnvironmentVariable( "PLANETARY_PROBE_OUT" );
var TOLERANCE = 1e-6;

function runConvertCase( c, P, r )
{
   var before = windowIds().length;
   Console.beginLog();
   var ok;
   try
   {
      ok = P.executeGlobal();
   }
   finally
   {
      var log = Console.endLog().toString();
   }
   if ( ok !== true )
      r.errors.push( "executeGlobal returned " + ok );
   if ( windowIds().length != before )
      r.errors.push( "a conversion opened an image window: " + windowIds().join( "," ) );
   if ( !File.exists( c.saveSerPath ) )
      r.errors.push( "no file written at " + c.saveSerPath );
   if ( File.exists( c.saveSerPath + ".partial" ) )
      r.errors.push( "the .partial file was left behind" );
   // expectConsole: every text present, each after the previous one.
   var from = 0;
   for ( var i = 0; i < c.expectConsole.length; ++i )
   {
      var at = log.indexOf( c.expectConsole[i], from );
      if ( at < 0 )
      {
         r.errors.push( "console lacks '" + c.expectConsole[i] + "'" +
                        (log.indexOf( c.expectConsole[i] ) >= 0 ? " after the previous expected text" : "") +
                        ": " + log.substring( 0, 900 ) );
         break;
      }
      from = at + c.expectConsole[i].length;
   }
   for ( var j = 0; j < c.consoleAbsent.length; ++j )
      if ( log.indexOf( c.consoleAbsent[j] ) >= 0 )
         r.errors.push( "console carries '" + c.consoleAbsent[j] + "'" );
}

function runErrorCase( c, P, r )
{
   var before = windowIds().length;
   var targetExisted = c.saveSerPath !== undefined && File.exists( c.saveSerPath );
   var threw = false, message = "", ok = undefined;
   Console.beginLog();
   try
   {
      ok = P.executeGlobal();
   }
   catch ( e )
   {
      threw = true;
      message = String( e );
   }
   var log = Console.endLog().toString();
   if ( threw )
   {
      r.via = "exception";
      if ( message.indexOf( c.expectError ) < 0 )
         r.errors.push( "threw '" + message + "', expected text '" + c.expectError + "'" );
   }
   else if ( ok === false )
   {
      r.via = "console";
      if ( log.indexOf( c.expectError ) < 0 )
         r.errors.push( "returned false; console lacks '" + c.expectError + "': " + log.substring( 0, 600 ) );
   }
   else
      r.errors.push( "expected a failure containing '" + c.expectError + "', but executeGlobal returned " + ok );
   if ( windowIds().length != before )
      r.errors.push( "a failed run left a new image window open: " + windowIds().join( "," ) );
   if ( c.saveSerPath !== undefined )
   {
      if ( targetExisted && !File.exists( c.saveSerPath ) )
         r.errors.push( "a failed conversion removed the existing " + c.saveSerPath );
      if ( !targetExisted && File.exists( c.saveSerPath ) )
         r.errors.push( "a failed conversion left " + c.saveSerPath + " behind" );
      if ( File.exists( c.saveSerPath + ".partial" ) )
         r.errors.push( "a failed conversion left the .partial file behind" );
   }
}

function runImageCase( c, P, r )
{
   var ok = P.executeGlobal();
   if ( ok !== true )
   {
      r.errors.push( "executeGlobal returned " + ok );
      return;
   }
   var w = ImageWindow.activeWindow;
   if ( w.isNull )
   {
      r.errors.push( "no active image window after executeGlobal" );
      return;
   }
   var v = w.mainView;
   var img = v.image;
   r.id = v.id;
   if ( v.id != c.expectId )
      r.errors.push( "window id " + v.id + ", expected " + c.expectId );
   if ( img.width != c.width || img.height != c.height || img.numberOfChannels != c.channels )
      r.errors.push( "geometry " + img.width + "x" + img.height + "x" + img.numberOfChannels +
                     ", expected " + c.width + "x" + c.height + "x" + c.channels );
   else
   {
      if ( img.bitsPerSample != 32 || !img.isReal )
         r.errors.push( "sample format " + img.bitsPerSample + "-bit real=" + img.isReal + ", expected 32-bit float" );
      for ( var i = 0; i < c.samples.length; ++i )
      {
         var s = c.samples[i];
         var got = img.sample( s.x, s.y, s.c );
         if ( Math.abs( got - s.v ) > TOLERANCE )
            r.errors.push( "sample(" + s.x + "," + s.y + "," + s.c + ") = " + got + ", expected " + s.v );
      }
   }
   var kws = w.keywords;
   checkKeywords( kws, c.keywords, c.absentKeywords, r.errors );
   if ( c.absentComments !== undefined )
      for ( var t = 0; t < c.absentComments.length; ++t )
         for ( var q = 0; q < kws.length; ++q )
            if ( kws[q].comment.indexOf( c.absentComments[t] ) >= 0 )
            {
               r.errors.push( "keyword comment contains '" + c.absentComments[t] + "': " + kws[q].name + " /" +
                              kws[q].comment );
               break;
            }
   w.forceClose();
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
            var P = new SERViewer;
            P.filePath = c.path;
            P.frameIndex = c.frame;
            P.debayer = c.debayer;
            if ( c.bayerPattern !== undefined )
            {
               var element = SERViewer.prototype[c.bayerPattern];
               if ( element === undefined )
                  throw new Error( "SERViewer has no enum element " + c.bayerPattern );
               P.bayerPattern = element;
            }
            if ( c.saveSerPath !== undefined )
               P.saveSerPath = c.saveSerPath;
            if ( c.expectError !== undefined )
               runErrorCase( c, P, r );
            else if ( c.saveSerPath !== undefined )
               runConvertCase( c, P, r );
            else
               runImageCase( c, P, r );
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
