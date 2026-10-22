// Planetary headless probes: helpers shared by probe-execute.js (SERViewer) and
// probe-stack.js (PlanetaryStack). Test-only, never shipped. ES5 only.

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

// value / comment are checked card by card; commentContains is checked on each
// run of consecutive same-name cards joined with single spaces, because long
// HISTORY text is split across cards at spaces.
function keywordFound( kws, want )
{
   var run = null;
   for ( var i = 0; i < kws.length; ++i )
   {
      var k = kws[i];
      if ( k.name != want.name )
      {
         run = null;
         continue;
      }
      if ( want.value !== undefined && k.strippedValue.trim() == want.value )
         return true;
      if ( want.comment !== undefined && k.comment.trim() == want.comment )
         return true;
      if ( want.commentContains !== undefined )
      {
         run = run === null ? k.comment.trim() : run + " " + k.comment.trim();
         if ( run.indexOf( want.commentContains ) >= 0 )
            return true;
      }
      if ( want.value === undefined && want.comment === undefined && want.commentContains === undefined )
         return true;
   }
   return false;
}

function describeKeywords( kws )
{
   var s = [];
   for ( var i = 0; i < kws.length; ++i )
      s.push( kws[i].name + "=" + kws[i].value + " /" + kws[i].comment );
   return s.join( " | " );
}

// Keyword checks shared by every image case: each expected keyword present,
// no HISTORY card over a FITS card's 72 characters of text, each absent
// keyword absent.
function checkKeywords( kws, keywords, absentKeywords, errors )
{
   for ( var j = 0; j < keywords.length; ++j )
      if ( !keywordFound( kws, keywords[j] ) )
         errors.push( "keyword " + JSON.stringify( keywords[j] ) + " not found in: " + describeKeywords( kws ) );
   for ( var h = 0; h < kws.length; ++h )
      if ( kws[h].name == "HISTORY" && kws[h].comment.length > 72 )
         errors.push( "HISTORY card longer than 72 characters (" + kws[h].comment.length + "): " + kws[h].comment );
   for ( var a = 0; a < absentKeywords.length; ++a )
      for ( var m = 0; m < kws.length; ++m )
         if ( kws[m].name == absentKeywords[a] )
         {
            errors.push( "keyword " + absentKeywords[a] + " present (" + kws[m].value + "), expected absent" );
            break;
         }
}

// No keyword comment contains any of `texts`, nor does any run of consecutive
// same-name cards joined with single spaces (long HISTORY text is split across
// cards at spaces, as keywordFound joins it).
function checkAbsentComments( kws, texts, errors )
{
   for ( var t = 0; t < texts.length; ++t )
   {
      var run = "", runName = null;
      for ( var q = 0; q < kws.length; ++q )
      {
         run = kws[q].name == runName ? run + " " + kws[q].comment.trim() : kws[q].comment.trim();
         runName = kws[q].name;
         if ( kws[q].comment.indexOf( texts[t] ) >= 0 || run.indexOf( texts[t] ) >= 0 )
         {
            errors.push( "keyword comment contains '" + texts[t] + "': " + kws[q].name + " /" + kws[q].comment );
            break;
         }
      }
   }
}
