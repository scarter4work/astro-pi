/*
 * AutoContrast — spike: can PixInsight ImageSolver plate-solve a press-release render?
 *
 * These references have no usable WCS (AVM unparseable, no published FoV), so tier 3
 * (§5.2) must solve them. We know the target position (M42) but NOT the image scale,
 * so we sweep a few field-of-view hints and take the first that solves.
 *
 * Writes a machine-readable result line to STATUS_FILE.
 */

/* CRITICAL: the astrometry headers use ES6 class expressions (`var DMath = class`),
 * which PixInsight's LEGACY JS engine rejects with
 *   "SyntaxError: class is a reserved identifier".
 * ImageSolver.js opts into V8 on its line 18; we must do the same or every
 * pjsr/astrometry include fails (silently, in headless mode). */
#engine v8

/* Replicate ImageSolver.js's own header exactly: the engine is NOT self-contained —
 * it needs these #defines and this include chain (SolverConfiguration extends
 * PersistentObject; SOLVER_SETTINGS_MODULE is defined here, not in the engine). */
#define VERSION "6.4.2"
#define TITLE "ImageSolver"
#define SOLVER_SETTINGS_MODULE "ImageSolver"
/* AstronomicalCatalogs.js needs a plain SETTINGS_MODULE (Catalog extends
 * PersistentObject and passes it to super()). ImageSolver.js defines it on line 36,
 * indented inside an #ifndef block. Without it: "ReferenceError: SETTINGS_MODULE is
 * not defined" at new VisiblePlanets(). */
#define SETTINGS_MODULE "ImageSolver"

#include <pjsr/astrometry/AstrometricMetadata.js>
#include <pjsr/astrometry/AstronomicalCatalogs.js>
#include <pjsr/astrometry/SearchCoordinatesDialog.js>
#include <pjsr/astrometry/CatalogDownloaderDialog.js>
#include <pjsr/astrometry/ProjectionConfigurationDialog.js>
#include <pjsr/astrometry/UtilityControls.js>
#include <pjsr/astrometry/VizierMirrorDialog.js>
#include <pjsr/controls/DateTimeEditor.js>
#include <pjsr/controls/GeodeticCoordinatesEditor.js>
#include "/opt/PixInsight/src/scripts/ImageSolver/ImageSolverDialog.js"
#include "/opt/PixInsight/src/scripts/ImageSolver/ImageSolverEngine.js"

var PROJECT     = "/home/scarter4work/projects/astro-pi/tools/autocontrast";
var IMAGE       = PROJECT + "/data/references/opo0205c.jpg";  // ground-based, star-rich
var STATUS_FILE = "/tmp/autocontrast_bridge/solve_spike.txt";

// M42 position hint (deg). We know WHAT the image is; we do not know its scale.
var HINT_RA  = 83.822;
var HINT_DEC = -5.391;

// Candidate fields of view (arcmin) across the frame's long axis.
var FOV_GUESSES = [ 20, 30, 45, 60, 90 ];

// No local XPSD star database is installed, so force the online VizieR catalog
// rather than CatalogMode.Automatic (which would look for a local server first).
var LOG = [];

function writeText( path, text ) {
   var f = new File;
   f.createForWriting( path );
   f.write( ByteArray.stringToUTF8( text ) );
   f.close();
}

function trySolve( window, fovArcmin ) {
   var w = window.mainView.image.width;
   var h = window.mainView.image.height;
   var longSide = Math.max( w, h );
   // arcsec/px -> deg/px
   var resolution = (fovArcmin * 60 / longSide) / 3600;

   var engine = new ImageSolver;
   engine.initialize( window, false /*prioritizeSettings*/ );

   /* Leave catalogMode = Automatic (the default). With no local XPSD database and a
    * sub-3-degree field, the engine auto-selects the ONLINE GaiaDR2 catalog — far
    * deeper than TYCHO-2 (mag<=11.5), which yields too few stars to align on. */
   engine.solverCfg.vizierServer     = "https://vizier.cds.unistra.fr/";
   engine.solverCfg.autoMagnitude    = true;
   engine.solverCfg.generateErrorImg = false;

   engine.metadata.ra         = HINT_RA;
   engine.metadata.dec        = HINT_DEC;
   engine.metadata.resolution = resolution;
   engine.metadata.width      = w;
   engine.metadata.height     = h;
   /* A press-release JPEG has no DATE-OBS, so observationTime stays null and
    * `new Position(null,"UTC")` throws. Seed J2000.0; proper motion over a few
    * years is negligible at these plate scales. */
   engine.metadata.observationTime = 2451545.0;   // JD for J2000.0
   engine.metadata.SaveParameters();
   engine.solverCfg.SaveParameters();

   console.writeln( format( "  trying FoV %.1f' -> %.4f\"/px ...",
                            fovArcmin, resolution * 3600 ) );
   if ( !engine.solveImage( window ) )
      return null;

   var md = new AstrometricMetadata;
   md.ExtractMetadata( window );
   if ( md.ra == null || md.resolution == null )
      return null;
   return md;
}

function main() {
   console.writeln( "<b>ImageSolver spike:</b> " + IMAGE );
   var window = ImageWindow.open( IMAGE )[0];
   if ( !window || !window.isWindow )
      throw new Error( "could not open image" );

   console.writeln( format( "image: %d x %d",
                    window.mainView.image.width, window.mainView.image.height ) );

   for ( var i = 0; i < FOV_GUESSES.length; ++i ) {
      var md = null;
      try {
         md = trySolve( window, FOV_GUESSES[i] );
         if ( md == null )
            LOG.push( "fov " + FOV_GUESSES[i] + "': solveImage returned false" );
      } catch ( e ) {
         // e may not be an Error object; never let logging itself throw.
         var em = (e && e.message) ? e.message : String( e );
         LOG.push( "fov " + FOV_GUESSES[i] + "': THREW " + em );
         console.warningln( "    solve threw: " + em );
      }
      if ( md != null ) {
         var scaleArcsec = md.resolution * 3600;
         console.noteln( format( "SOLVED  ra=%.5f dec=%.5f scale=%.4f\"/px",
                                 md.ra, md.dec, scaleArcsec ) );
         writeText( STATUS_FILE, format(
            "SOLVED ra=%.6f dec=%.6f scale_arcsec=%.6f width=%d height=%d fov_hint=%.1f\n",
            md.ra, md.dec, scaleArcsec,
            window.mainView.image.width, window.mainView.image.height, FOV_GUESSES[i] ) );
         window.forceClose();
         return;
      }
   }

   window.forceClose();
   writeText( STATUS_FILE, "UNSOLVED\n" + LOG.join( "\n" ) + "\n" );
   console.criticalln( "UNSOLVED after all FoV hints" );
}

try {
   main();
} catch ( e ) {
   var msg = (e && e.message) ? e.message : String( e );
   var stack = (e && e.stack) ? "\n" + e.stack : "";
   writeText( STATUS_FILE, "ERROR " + msg + stack + "\nlog:\n" + LOG.join( "\n" ) + "\n" );
   console.criticalln( "ERROR: " + msg );
}
