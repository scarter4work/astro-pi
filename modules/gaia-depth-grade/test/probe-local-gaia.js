// GaiaDepthGrade probe `local_gaia_equals_gaia_process` (plan Task 12). Test-only, never shipped.
//
// 1. The test-only GaiaDepthGradeProbe process (case local_gaia) discovers the Gaia database
//    files, searches the cone with LocalGaiaSource (pcl::GaiaDatabaseFile) and writes the rows.
// 2. PixInsight's own Gaia process searches the same files and cone with the parameters the
//    product used (_searchGaia in gaia_depth_grade_lib.jsh).
// 3. The two row multisets are compared exactly on (ra, dec, parallax, G, BP, RP, flags).
// Env: GDG_PROBE_OUT, GDG_CONE ("<ra> <dec> <radius_deg> <magLimit>").
//
// ES5 only (an ES6 class anywhere kills a -r= script before line 1 runs).

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );
var CONE = getEnvironmentVariable( "GDG_CONE" );

function write( obj )
{
   File.writeTextFile( OUT, JSON.stringify( obj ) + "\n" );
}

function looksLikeSP( paths )
{
   for ( var i = 0; i < paths.length; ++i )
      if ( /gdr3sp|dr3sp|[_\-.]sp[_\-.]/i.test( paths[i] ) )
         return true;
   return false;
}

function gaiaProcessSearch( paths, ra, dec, radius, mag )
{
   var P = new Gaia;
   var rows = [];
   for ( var i = 0; i < paths.length; ++i )
      rows.push( [ paths[i] ] );
   P.databaseFilePaths = rows;
   var order = looksLikeSP( paths ) ? [ Gaia.prototype.DataRelease_3_SP, Gaia.prototype.DataRelease_3 ]
                                    : [ Gaia.prototype.DataRelease_3, Gaia.prototype.DataRelease_3_SP ];
   for ( var k = 0; k < order.length; ++k )
   {
      P.dataRelease = order[k];
      P.command = "configure";
      P.executeGlobal();
      if ( !P.isValid )
         continue;
      P.command = "search";
      P.centerRA = ra; P.centerDec = dec; P.radius = radius;
      P.magnitudeLow = -5.0; P.magnitudeHigh = mag;
      P.sourceLimit = 4294967295;
      P.requiredFlags = 0; P.inclusionFlags = 0; P.exclusionFlags = 0;
      P.verbosity = 0; P.generateTextOutput = false;
      if ( !P.executeGlobal() )
         continue;
      if ( P.sources.length > 0 )
         return { release: order[k], sources: P.sources };
   }
   throw new Error( "the Gaia process returned no sources for any data release" );
}

function key( a ) { return a.join( "," ); }

function main()
{
   if ( !OUT )
      throw new Error( "GDG_PROBE_OUT is not set" );
   var c = CONE.split( " " );
   var ra = parseFloat( c[0] ), dec = parseFloat( c[1] ), radius = parseFloat( c[2] ), mag = parseFloat( c[3] );
   var probeOut = OUT + ".cpp.json", rowsFile = OUT + ".rows.tsv";

   var Q = new GaiaDepthGradeProbe;
   Q.probeCase = "local_gaia";
   Q.probeArgs = CONE + " " + rowsFile;
   Q.outputPath = probeOut;
   Q.executeGlobal();
   var cpp = JSON.parse( File.readTextFile( probeOut ) );
   if ( !cpp.ok )
      return write( { ok: false, error: "local_gaia probe: " + cpp.error } );

   // C++ rows: %.17g round-trips, so parseFloat gives the exact doubles. Rows are paired on the
   // stored catalogue values (parallax, G, BP, RP, flags: exact float data) and then compared on
   // position, which both sides compute from the encoded offsets (XPSD Unproject): the core's
   // build and ours may round that differently in the last bit.
   var byCat = {}, nMine = 0;
   var lines = File.readTextFile( rowsFile ).split( "\n" );
   for ( var i = 0; i < lines.length; ++i )
   {
      if ( lines[i].length == 0 )
         continue;
      var f = lines[i].split( "\t" );
      var row = [ parseFloat( f[0] ), parseFloat( f[1] ), parseFloat( f[2] ), parseFloat( f[3] ),
                  parseFloat( f[4] ), parseFloat( f[5] ), parseInt( f[6] ) ];
      var kc = key( row.slice( 2 ) );
      if ( !byCat[kc] )
         byCat[kc] = [];
      byCat[kc].push( row );
      ++nMine;
   }

   var T0 = Date.now();
   var g = gaiaProcessSearch( cpp.paths, ra, dec, radius, mag );
   var gaiaSeconds = (Date.now() - T0)/1000;
   var unmatched = 0, exact = 0, nGaia = 0, maxPosDelta = 0, sample = [];
   for ( var j = 0; j < g.sources.length; ++j )
   {
      var s = g.sources[j];
      // Gaia.sources: [ra, dec, parallax, pmRA, pmDec, magG, magBP, magRP, flags, ...]
      var r = [ s[0], s[1], s[2], s[5], s[6], s[7], s[8] ];
      ++nGaia;
      var list = byCat[ key( r.slice( 2 ) ) ];
      var best = -1, bestD = 1e300;
      if ( list )
         for ( var q = 0; q < list.length; ++q )
         {
            var d = Math.max( Math.abs( list[q][0] - r[0] ), Math.abs( list[q][1] - r[1] ) );
            if ( d < bestD ) { bestD = d; best = q; }
         }
      if ( best < 0 || bestD > 1e-9 )
      {
         ++unmatched;
         if ( sample.length < 4 )
            sample.push( "gaia " + key( r ) );
         continue;
      }
      if ( bestD == 0 )
         ++exact;
      if ( bestD > maxPosDelta )
         maxPosDelta = bestD;
      list.splice( best, 1 );
   }
   var extraInCpp = 0;
   for ( var m in byCat )
      extraInCpp += byCat[m].length;
   write( { ok: true, nCpp: nMine, nGaia: nGaia, unmatched: unmatched, extraInCpp: extraInCpp, exact: exact,
            maxPosDeltaDeg: maxPosDelta, sampleUnmatched: sample, release: g.release, gaiaSeconds: gaiaSeconds,
            cpp: cpp } );
}

try
{
   main();
}
catch ( e )
{
   write( { ok: false, error: "probe-local-gaia.js: " + e.toString() } );
}
