// Planetary headless probe: runs SERViewer.executeGlobal() on each fixture
// case and checks the window it opens. Test-only, never shipped.
//
// Input:  $PLANETARY_PROBE_IN  -- JSON written by make-fixtures:
//   { "cases": [ { name, path, frame, debayer,
//                  expectId, width, height, channels,
//                  samples: [ {x, y, c, v} ],                 (tolerance 1e-6)
//                  keywords: [ {name, value | comment | commentContains} ],
//                  absentKeywords: [ name ] }
//              | { name, path, frame, debayer, expectError } ] }
// Output: $PLANETARY_PROBE_OUT -- { "ok": bool, "cases": [ {name, ok, errors[]} ] }
//
// An error case must fail loudly with the expected text: either executeGlobal
// throws it, or it returns false and the Process Console carries it (the
// output records which), and in both cases no image window may be left open.
//
// ES5 only: an ES6 `class` anywhere kills a -r= script before line 1 runs.

var PROBE_IN = getEnvironmentVariable( "PLANETARY_PROBE_IN" );
var PROBE_OUT = getEnvironmentVariable( "PLANETARY_PROBE_OUT" );
var TOLERANCE = 1e-6;

function windowIds()
{
   var ids = [];
   var ws = ImageWindow.windows;
   for ( var i = 0; i < ws.length; ++i )
      ids.push( ws[i].mainView.id );
   return ids;
}

function closeAllWindows()
{
   var ws = ImageWindow.windows;
   for ( var i = 0; i < ws.length; ++i )
      ws[i].forceClose();
}

function keywordMatches( k, want )
{
   if ( k.name != want.name )
      return false;
   if ( want.value !== undefined )
      return k.strippedValue.trim() == want.value;
   if ( want.comment !== undefined )
      return k.comment.trim() == want.comment;
   if ( want.commentContains !== undefined )
      return k.comment.indexOf( want.commentContains ) >= 0;
   return true;
}

function describeKeywords( kws )
{
   var s = [];
   for ( var i = 0; i < kws.length; ++i )
      s.push( kws[i].name + "=" + kws[i].value + " /" + kws[i].comment );
   return s.join( " | " );
}

function runErrorCase( c, P, r )
{
   var before = windowIds().length;
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
   for ( var j = 0; j < c.keywords.length; ++j )
   {
      var want = c.keywords[j];
      var found = false;
      for ( var k = 0; k < kws.length && !found; ++k )
         found = keywordMatches( kws[k], want );
      if ( !found )
         r.errors.push( "keyword " + JSON.stringify( want ) + " not found in: " + describeKeywords( kws ) );
   }
   for ( var a = 0; a < c.absentKeywords.length; ++a )
      for ( var m = 0; m < kws.length; ++m )
         if ( kws[m].name == c.absentKeywords[a] )
         {
            r.errors.push( "keyword " + c.absentKeywords[a] + " present (" + kws[m].value + "), expected absent" );
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
            if ( c.expectError !== undefined )
               runErrorCase( c, P, r );
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
