// GaiaDepthGrade id probe (plan Task 2). Test-only, never shipped.
//
// Records, for the running PixInsight, the process and parameter ids the
// module's later tasks drive through ProcessInstance, and writes them as JSON
// to $GDG_PROBE_OUT:
//   - StarXTerminator: whether the process exists, and its parameter ids;
//   - Gaia: parameter ids;
//   - StarAlignment: parameter ids and the enumeration / outputData column
//     constants ImageSolverEngine.js (l.378-440) uses;
//   - whether a process with id ImageSolver or BlindSolver exists (none expected);
//   - the PixInsight core version.
// Module load and module id are checked by run-probe-ids.sh (the process map
// and the core's own load report): a module with no processes is invisible here.
//
// ES5 only: an ES6 `class` anywhere kills a -r= script before line 1 runs.

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );

// eval() below only ever sees the fixed process ids written in this file
// (PJSR has no other way to name a process object from a string); no
// external input reaches it.

function processExists( id )
{
   try
   {
      return eval( "typeof " + id ) == "function";
   }
   catch ( e )
   {
      return false;
   }
}

// Parameter ids of a process, in the order the core serialises them.
function parameterIds( id )
{
   var P = eval( "new " + id );
   var src = P.toSource( "JavaScript", "P" );
   var ids = [];
   var re = /^\s*P\.([A-Za-z_][A-Za-z0-9_]*)\s*=/gm, m;
   while ( (m = re.exec( src )) !== null )
      if ( ids.indexOf( m[1] ) < 0 )
         ids.push( m[1] );
   return ids;
}

function constants( id, names )
{
   // As ImageSolverEngine.js reads them (StarAlignment.OutputMatrix); an
   // instance is consulted too, in case the engine exposes them only there.
   var C = eval( id ), I = eval( "new " + id ), r = {};
   for ( var i = 0; i < names.length; ++i )
   {
      var n = names[i], v = C[n];
      if ( v === undefined )
         v = I[n];
      r[n] = (typeof v == "number") ? v : ((v === undefined || v === null) ? null : typeof v + ":" + v);
   }
   return r;
}

// Module files mapped into this PixInsight process whose name contains `name`:
// a -m= module the core rejected is unloaded before any -r= script runs.
function mappedModules( name )
{
   var found = [];
   var lines = File.readLines( "/proc/self/maps" );
   for ( var i = 0; i < lines.length; ++i )
   {
      var f = lines[i].replace( /^\S+\s+\S+\s+\S+\s+\S+\s+\S+\s*/, "" );
      if ( f.indexOf( name ) >= 0 && found.indexOf( f ) < 0 )
         found.push( f );
   }
   return found;
}

var result = { ok: false };
try
{
   result.mappedModules = mappedModules( "GaiaDepthGrade-pxm.so" );
   result.core = coreVersionMajor + "." + coreVersionMinor + "." + coreVersionRelease + "-" + coreVersionRevision
               + " build " + coreVersionBuild;
   result.starXTerminator = processExists( "StarXTerminator" );
   if ( result.starXTerminator )
   {
      result.starXTerminatorParameters = parameterIds( "StarXTerminator" );
      // Default values too: the split relies on the stars image being produced.
      result.starXTerminatorDefaults = eval( "new StarXTerminator" ).toSource( "JavaScript", "P" )
                                          .split( "\n" ).filter( function( l ) { return /^\s*P\./.test( l ); } )
                                          .map( function( l ) { return l.trim(); } );
   }
   result.gaia = processExists( "Gaia" );
   if ( result.gaia )
      result.gaiaParameters = parameterIds( "Gaia" );
   result.starAlignment = processExists( "StarAlignment" );
   if ( result.starAlignment )
   {
      result.starAlignmentParameters = parameterIds( "StarAlignment" );
      result.starAlignmentConstants = constants( "StarAlignment",
         [ "OutputMatrix", "NoIntersection", "Always",
           "outputData_totalPairMatches", "outputData_referenceStarX", "outputData_referenceStarY",
           "outputData_targetStarX", "outputData_targetStarY" ] );
   }
   result.imageSolverProcess = processExists( "ImageSolver" );
   result.blindSolverProcess = processExists( "BlindSolver" );
   result.ok = true;
}
catch ( e )
{
   result.error = "" + e;
}
File.writeTextFile( OUT, JSON.stringify( result, null, 2 ) + "\n" );
