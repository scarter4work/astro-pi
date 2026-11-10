// GaiaDepthGrade probes `solve_*` (plan Tasks 14-15). Test-only, never shipped.
//
// Opens a master (never saved), optionally clears its astrometric solution and strips its
// acquisition metadata, solves it with the module's native solver (GaiaDepthGradeProbe case
// `solve`), and compares the solution PixInsight regenerates from what the solver wrote with
// ImageSolver 6.6's own (the samples written by solver-reference.js): for every reference sample
// (x, y) -> (ra, dec), the pixel residual |celestialToImage(ra, dec) - (x, y)|.
//
// Env:
//   GDG_PROBE_OUT       output JSON
//   GDG_SOLVE_MASTER    the image file
//   GDG_SOLVE_REF       solver-reference.js output for the same master ("" = no comparison)
//   GDG_SOLVE_ARGS      extra `solve` arguments (gaia=..., index=..., blind=..., distortion=...)
//   GDG_SOLVE_CLEAR     "1": clear the existing astrometric solution first
//   GDG_SOLVE_STRIP     "1": also remove every coordinate / scale keyword and property, so the
//                       image provides no seed at all
//   GDG_SOLVE_CONFIGURE_INDEX  an index path to select in this test slot's Gaia preferences
//                       ("none": clear it) before solving
//   GDG_SOLVE_GAIA      Gaia DR3 files (':'-separated) to configure in this slot's Gaia process
//
// ES5 only (an ES6 class anywhere kills a -r= script before line 1 runs).

#include <pjsr/UndoFlag.jsh>

var OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );

function write( obj )
{
   File.writeTextFile( OUT, JSON.stringify( obj ) + "\n" );
}

var STRIP_KEYWORDS = [ "RA", "DEC", "OBJCTRA", "OBJCTDEC", "FOCALLEN", "XPIXSZ", "YPIXSZ", "PIXSIZE", "PIXSIZE1",
                       "PIXSIZE2", "CTYPE1", "CTYPE2", "CRVAL1", "CRVAL2", "CRPIX1", "CRPIX2", "CD1_1", "CD1_2",
                       "CD2_1", "CD2_2", "CDELT1", "CDELT2", "CROTA1", "CROTA2", "PC1_1", "PC1_2", "PC2_1",
                       "PC2_2", "EQUINOX", "RADESYS", "LONPOLE", "LATPOLE", "PV1_1", "PV1_2", "SCALE", "PLTSOLVD",
                       "OBJCTALT", "OBJCTAZ", "TELESCOP" ];
var STRIP_PROPERTIES = [ "Observation:Center:RA", "Observation:Center:Dec", "Observation:Center:X",
                         "Observation:Center:Y", "Instrument:Telescope:FocalLength", "Instrument:Sensor:XPixelSize",
                         "Instrument:Sensor:YPixelSize", "Instrument:Telescope:Aperture" ];

function stripSeed( w )
{
   var kept = [];
   var k = w.keywords;
   for ( var i = 0; i < k.length; ++i )
      if ( STRIP_KEYWORDS.indexOf( k[i].name ) < 0 )
         kept.push( k[i] );
   w.keywords = kept;
   for ( var j = 0; j < STRIP_PROPERTIES.length; ++j )
      if ( w.mainView.hasProperty( STRIP_PROPERTIES[j] ) )
         w.mainView.deleteProperty( STRIP_PROPERTIES[j] );
}

// Least squares z' = a z + b over complex numbers (centred): the scale ratio |a| and rotation
// arg(a) between the reference sample positions z and our positions z' of the same sky points.
function similarity( ref, ours )
{
   var n = ref.length, mx = 0, my = 0, nx = 0, ny = 0;
   for ( var i = 0; i < n; ++i ) { mx += ref[i][0]; my += ref[i][1]; nx += ours[i][0]; ny += ours[i][1]; }
   mx /= n; my /= n; nx /= n; ny /= n;
   var sre = 0, sim = 0, s2 = 0;
   for ( var j = 0; j < n; ++j )
   {
      var zx = ref[j][0] - mx, zy = ref[j][1] - my, wx = ours[j][0] - nx, wy = ours[j][1] - ny;
      sre += zx*wx + zy*wy;  // Re( conj(z) w )
      sim += zx*wy - zy*wx;  // Im( conj(z) w )
      s2 += zx*zx + zy*zy;
   }
   var ar = sre/s2, ai = sim/s2;
   return { scaleRatio: Math.sqrt( ar*ar + ai*ai ), rotationDeg: Math.atan2( ai, ar )*180/Math.PI };
}

function main()
{
   var master = getEnvironmentVariable( "GDG_SOLVE_MASTER" );
   var refPath = getEnvironmentVariable( "GDG_SOLVE_REF" );
   var gaia = getEnvironmentVariable( "GDG_SOLVE_GAIA" );
   var cfgIndex = getEnvironmentVariable( "GDG_SOLVE_CONFIGURE_INDEX" );

   if ( gaia || cfgIndex )
   {
      var G = new Gaia;
      if ( gaia )
      {
         var rows = [];
         var files = gaia.split( ":" );
         for ( var f = 0; f < files.length; ++f )
            if ( files[f].length > 0 )
               rows.push( [ files[f] ] );
         G.databaseFilePaths = rows;
      }
      G.dataRelease = 3; // Gaia.DataRelease_3 (enums.js probe)
      if ( cfgIndex )
         G.astrometricIndexFilePath = (cfgIndex == "none") ? "" : cfgIndex;
      G.command = "configure";
      G.executeGlobal();
   }

   var window = ImageWindow.open( master, "", "non-interactive" )[0];
   if ( !window || window.isNull )
      return write( { ok: false, error: "cannot open " + master } );
   var result = { ok: false };
   try
   {
      var W = window.mainView.image.width, H = window.mainView.image.height;
      result.width = W;
      result.height = H;
      window.mainView.beginProcess( UndoFlag_NoSwapFile );
      if ( getEnvironmentVariable( "GDG_SOLVE_CLEAR" ) == "1" || getEnvironmentVariable( "GDG_SOLVE_STRIP" ) == "1" )
         window.clearAstrometricSolution();
      if ( getEnvironmentVariable( "GDG_SOLVE_STRIP" ) == "1" )
         stripSeed( window );
      window.mainView.endProcess();
      result.hadSolution = window.hasAstrometricSolution;

      var Q = new GaiaDepthGradeProbe;
      Q.probeCase = "solve";
      Q.probeArgs = "window=" + window.mainView.id + " " + (getEnvironmentVariable( "GDG_SOLVE_ARGS" ) || "");
      Q.outputPath = OUT + ".solve.json";
      Q.executeGlobal();
      var s = JSON.parse( File.readTextFile( OUT + ".solve.json" ) );
      if ( !s.ok )
         return write( { ok: false, error: "solve probe: " + s.error } );
      result.solve = s;
      result.hasSolution = window.hasAstrometricSolution;

      if ( s.solved && refPath )
      {
         var ref = JSON.parse( File.readTextFile( refPath ) );
         var res = [], refPts = [], ourPts = [], sum2 = 0, max = 0;
         for ( var i = 0; i < ref.samples.length; ++i )
         {
            var r = ref.samples[i];
            var q = window.celestialToImage( r[2], r[3] );
            var dx = q.x - r[0], dy = q.y - r[1], d = Math.sqrt( dx*dx + dy*dy );
            res.push( [ r[0], r[1], dx, dy, d ] );
            refPts.push( [ r[0], r[1] ] );
            ourPts.push( [ q.x, q.y ] );
            sum2 += d*d;
            if ( d > max ) max = d;
         }
         var pick = function( x, y )
         {
            for ( var k = 0; k < res.length; ++k )
               if ( Math.abs( res[k][0] - x ) < 1e-6 && Math.abs( res[k][1] - y ) < 1e-6 )
                  return res[k][4];
            return null;
         };
         var sim = similarity( refPts, ourPts );
         var c = window.imageToCelestial( W/2, H/2 );
         var rc = ref.samples[ref.samples.length - 1]; // the centre sample
         var cosd = Math.cos( rc[3]*Math.PI/180 );
         result.gate = {
            samples: res.length,
            rmsPx: Math.sqrt( sum2/res.length ),
            maxPx: max,
            centerPx: pick( W/2, H/2 ),
            cornersPx: [ pick( 0, 0 ), pick( W, 0 ), pick( 0, H ), pick( W, H ) ],
            centerArcsec: Math.sqrt( Math.pow( (c.x - rc[2])*cosd, 2 ) + Math.pow( c.y - rc[3], 2 ) )*3600,
            scaleErrorPct: (1/sim.scaleRatio - 1)*100,
            rotationDeg: sim.rotationDeg,
            referenceRmsPx: ref.rmsPx,
            referenceSeconds: ref.seconds,
            referenceMatched: ref.matchedStars
         };
         result.residuals = res;
      }
      result.ok = true;
   }
   catch ( e )
   {
      result.ok = false;
      result.error = "probe-solve.js: " + e.toString();
   }
   finally
   {
      window.forceClose();
   }
   write( result );
}

try { main(); }
catch ( e ) { write( { ok: false, error: "probe-solve.js: " + e.toString() } ); }
