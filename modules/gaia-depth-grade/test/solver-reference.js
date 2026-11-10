#engine v8
// GaiaDepthGrade solver gate reference (plan Tasks 14-15). Test-only, never shipped.
//
// Runs PixInsight's own ImageSolver 6.6 (the 1.9.6 script library, PCL License 2.0) headless on
// one master, with every ImageSolver setting at its default (useDefaultSettings) and the Gaia
// process configured, in this test settings slot only, with the database files the C++ solver
// uses. Writes the reference solution as image -> celestial samples over a grid that includes the
// image centre and the four corners, plus the RMS error of the final star matching.
//
// Env:
//   GDG_PROBE_OUT    output JSON
//   GDG_REF_MASTER   the image file (opened read-only; never saved)
//   GDG_REF_GAIA     Gaia DR3 .xpsd files, ':'-separated
//   GDG_REF_CLEAR    "1": clear the image's existing astrometric solution first, so ImageSolver
//                    starts from the acquisition metadata (RA/DEC, focal length, pixel size)
//   GDG_REF_GRID     "nx ny" sample grid (default "17 9")
//
// #engine v8: ImageSolver 6.6 is ES6 (classes); the default engine cannot load it.

#define SETTINGS_MODULE "GDGSolverReference"
#define USE_SOLVER_LIBRARY true
#include "/opt/PixInsight/src/scripts/ImageSolver/ImageSolver.js"

function main()
{
   const OUT = getEnvironmentVariable( "GDG_PROBE_OUT" );
   const result = { ok: false };
   let window = null;
   try
   {
      const master = getEnvironmentVariable( "GDG_REF_MASTER" );
      const gaia = getEnvironmentVariable( "GDG_REF_GAIA" ).split( ":" ).filter( p => p.length > 0 );
      const grid = (getEnvironmentVariable( "GDG_REF_GRID" ) || "17 9").split( " " ).map( s => parseInt( s ) );

      // Gaia: configure the database files in this (test) slot.
      const G = new Gaia;
      G.databaseFilePaths = gaia.map( p => [ p ] );
      G.dataRelease = Gaia.DataRelease_3;
      G.command = "configure";
      G.executeGlobal();
      G.command = "get-info";
      G.executeGlobal();
      if ( !G.isValid )
         throw new Error( "the Gaia process is not valid after configure" );

      window = ImageWindow.open( master, "", "non-interactive" )[0];
      if ( !window || window.isNull )
         throw new Error( "cannot open " + master );
      const W = window.mainView.image.width, H = window.mainView.image.height;
      result.width = W;
      result.height = H;
      result.keywords = window.keywords.map( k => [ k.name, k.value ] );
      if ( getEnvironmentVariable( "GDG_REF_CLEAR" ) == "1" )
      {
         window.mainView.beginProcess( UndoFlag.NoSwapFile );
         window.clearAstrometricSolution();
         window.mainView.endProcess();
         result.cleared = !window.hasAstrometricSolution;
      }

      const engine = new ImageSolver;
      engine.useDefaultSettings = true;
      engine.noHistoryState = false;
      engine.interactive = false;
      engine.initialize( window, false/*prioritizeSettings*/ );
      result.seed = { ra: engine.metadata.ra, dec: engine.metadata.dec, resolutionDeg: engine.metadata.resolution,
                      focal: engine.metadata.focal, xpixsz: engine.metadata.xpixsz,
                      observationTime: engine.metadata.observationTime };

      // The RMS error of the final solution: optimizeSolution() returns the metadata of its best
      // iteration; record the star matching of each metadata to report the returned one's.
      const matched = [];
      const matchStars = engine.matchStars.bind( engine );
      engine.matchStars = ( w, m ) => { const s = matchStars( w, m ); matched.push( { m: m, s: s } ); return s; };

      const t0 = Date.now();
      engine.solveImage( window );
      result.seconds = (Date.now() - t0)/1000;
      result.catalog = engine.catalogName;
      result.limitMagnitude = engine.limitMagnitude;
      for ( const r of matched )
         if ( r.m === engine.metadata )
         {
            result.rmsPx = r.s.rms;
            result.matchedStars = r.s.numValid;
         }
      result.resolutionArcsec = engine.metadata.resolution*3600;
      result.summary = window.astrometricSolutionSummary();

      const samples = [];
      for ( let j = 0; j < grid[1]; ++j )
         for ( let i = 0; i < grid[0]; ++i )
         {
            const x = W*i/(grid[0] - 1), y = H*j/(grid[1] - 1);
            const q = window.imageToCelestial( x, y );
            samples.push( [ x, y, q.x, q.y ] );
         }
      const c = window.imageToCelestial( W/2, H/2 );
      samples.push( [ W/2, H/2, c.x, c.y ] );
      result.samples = samples;
      result.ok = true;
   }
   catch ( ex )
   {
      result.error = ex.toString();
   }
   finally
   {
      if ( window && !window.isNull )
         window.forceClose();
   }
   File.writeTextFile( OUT, JSON.stringify( result ) + "\n" );
}

main();
