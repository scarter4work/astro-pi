// Gaia Depth Grade — thin PJSR harness wrapper.
// Orchestration only: solve, split stars, hand pixels to the Python core, recombine.
//
// Interactive: open a master and run this script on the active window.
//
// Headless (validated on PixInsight 1.9.4): run a controlled Xvfb you keep alive,
// pass the image via env, force a new instance, and do NOT use --force-exit (it
// exits before -r scripts run). PI's console does not reach stdout — read errors
// by screenshotting the GUI. Detect completion via the output file, then kill PI.
//   Xvfb :95 -screen 0 1600x1000x24 &
//   DISPLAY=:95 GAIA_DEPTH_INPUT=/path/master.xisf GAIA_DEPTH_OUTPUT=/path/out.xisf \
//     /opt/PixInsight/bin/PixInsight.sh -n --automation-mode \
//       --run=/abs/path/pjsr/gaia_depth_grade.js
//
// ImageSolver is a script-library object, not a core process. Including it with
// USE_SOLVER_LIBRARY exposes the class instead of launching its GUI main(), but
// that flag ALSO suppresses ImageSolver's own dependency includes — so we must
// pull in WCSmetadata/AstronomicalCatalogs first (and define SETTINGS_MODULE /
// STAR_CSV_FILE), exactly as AdP/AperturePhotometry.js does.
#define SETTINGS_MODULE "SOLVER"
#define STAR_CSV_FILE   (File.systemTempDirectory + format( "/stars-%03d.csv", CoreApplication.instance ))
#include <pjsr/DataType.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/UndoFlag.jsh>
#include <pjsr/TextAlign.jsh>
#include "/opt/PixInsight/src/scripts/AdP/WCSmetadata.jsh"
#include "/opt/PixInsight/src/scripts/AdP/AstronomicalCatalogs.jsh"
#define USE_SOLVER_LIBRARY true
#include "/opt/PixInsight/src/scripts/AdP/ImageSolver.js"

#define PY_BIN   "/home/scarter4work/projects/gaia-depth-grade/.venv/bin/python"
#define PKG      "gaia_depth_grade.cli"
#define TMP_DIR  "/tmp/gaia_depth_grade"

// ExternalProcess(commandLineString) starts a shell command and runs it; the
// constructor wants a single string (an array throws "String expected"), and
// p.start(program,args) returns a non-bool here, so use the canonical form:
// construct with a quoted command line, waitForFinished(), check exitCode.
function run(cmd) {
   var p = new ExternalProcess(cmd);
   p.waitForFinished();
   if (p.exitCode != 0)
      throw new Error("command failed (exit " + p.exitCode + "): " + cmd +
                      "\n" + p.stderr);
}

// Write standard FITS WCS keywords (CTYPE/CRVAL/CD) from the solver's solution
// directly onto a window's keyword list. We cannot use metadata.SaveKeywords():
// it calls UpdateWCSKeywords() WITHOUT generate=true (so it strips WCS keywords
// without re-emitting them), and even the generate path routes through a
// modifyKeywords() helper that discards its result and never appends new names.
// PI 1.9.x keeps the solution as XISF properties, which FITS cannot carry — so
// astropy.wcs sees nothing. Appending the keywords ourselves is the reliable path.
function writeWcsKeywords(win, metadata) {
   var wcs = metadata.GetWCSvalues();
   var kw = win.keywords;
   function add(n, v, c) { kw.push(new FITSKeyword(n, v, c)); }
   add("RADESYS", "'" + metadata.referenceSystem + "'", "Reference system of celestial coordinates");
   add("CTYPE1", wcs.ctype1, "Axis1 projection");
   add("CTYPE2", wcs.ctype2, "Axis2 projection");
   add("CRPIX1", format("%.8f", wcs.crpix1), "Axis1 reference pixel");
   add("CRPIX2", format("%.8f", wcs.crpix2), "Axis2 reference pixel");
   if (wcs.crval1 != null) add("CRVAL1", format("%.16f", wcs.crval1), "Axis1 reference value");
   if (wcs.crval2 != null) add("CRVAL2", format("%.16f", wcs.crval2), "Axis2 reference value");
   if (wcs.pv1_1 != null) add("PV1_1", format("%.16f", wcs.pv1_1), "Native longitude of the reference point");
   if (wcs.pv1_2 != null) add("PV1_2", format("%.16f", wcs.pv1_2), "Native latitude of the reference point");
   if (wcs.lonpole != null) add("LONPOLE", format("%.16f", wcs.lonpole), "Longitude of the celestial pole");
   if (wcs.latpole != null) add("LATPOLE", format("%.16f", wcs.latpole), "Latitude of the celestial pole");
   add("CD1_1", format("%.16f", wcs.cd1_1), "Scale matrix (1,1)");
   add("CD1_2", format("%.16f", wcs.cd1_2), "Scale matrix (1,2)");
   add("CD2_1", format("%.16f", wcs.cd2_1), "Scale matrix (2,1)");
   add("CD2_2", format("%.16f", wcs.cd2_2), "Scale matrix (2,2)");
   win.keywords = kw;
}

// StarXTerminator creates a separate stars window. Capture it by diffing the open
// window set rather than trusting a fixed "<id>_stars" naming convention.
function splitStars(view) {
   var before = {};
   var ws = ImageWindow.windows;
   for (var i = 0; i < ws.length; ++i)
      before[ws[i].mainView.id] = true;

   var sxt = new StarXTerminator;
   sxt.stars = true;       // emit the stars-only image (required, or no stars window)
   sxt.unscreen = true;    // produce a linear stars image
   if (!sxt.executeOn(view))   // modifies `view` in place -> starless
      throw new Error("StarXTerminator failed on " + view.id);

   ws = ImageWindow.windows;
   for (var j = 0; j < ws.length; ++j)
      if (!before[ws[j].mainView.id]) {
         console.noteln("stars window: " + ws[j].mainView.id);
         return ws[j];
      }
   throw new Error("StarXTerminator produced no new stars window");
}

function gradeWindow(window) {
   if (!File.directoryExists(TMP_DIR))   // createDirectory throws if it exists
      File.createDirectory(TMP_DIR, true);
   var view = window.mainView;

   // 1. Plate-solve. Init() seeds from the image's own keywords; SolveImage()
   //    takes the ImageWindow (not a view) and refines the solution in metadata.
   var solver = new ImageSolver;
   solver.Init(window, false);
   if (!solver.SolveImage(window))
      throw new Error("plate solve failed for " + view.id);

   // 2. Split stars/starless with StarXTerminator.
   var starsWin = splitStars(view);
   var starless = window;          // SXT leaves starless in place

   // Stamp the solved WCS onto the stars layer so the Python core (astropy.wcs)
   // can read it from the exported FITS. SXT preserves geometry, so the master's
   // solution applies pixel-for-pixel to the stars image.
   writeWcsKeywords(starsWin, solver.metadata);

   // 3. Export stars layer (+WCS) to FITS for the Python core.
   var inPath  = TMP_DIR + "/stars_in.fits";
   var outPath = TMP_DIR + "/stars_out.fits";
   if (!starsWin.saveAs(inPath, false, false, false, false))
      throw new Error("failed to save stars FITS: " + inPath);

   // 4. Run the Python depth grade (quote paths for the shell).
   run("\"" + PY_BIN + "\" -m " + PKG + " grade \"" + inPath + "\" \"" + outPath + "\"");

   // 5. Read the modulated stars layer back.
   var graded = ImageWindow.open(outPath)[0];

   // 6. Recombine modulated stars over starless via screen blend.
   //    Screen: result = 1 - (1-A)*(1-B) == ~((~A)*(~B)).
   var PM = new PixelMath;
   PM.expression = "~((~" + starless.mainView.id + ") * (~" + graded.mainView.id + "))";
   PM.createNewImage = true;
   PM.newImageId = view.id + "_depthgraded";
   PM.executeOn(starless.mainView);
   return ImageWindow.windowById(PM.newImageId);
}

function resolveTarget() {
   var w = ImageWindow.activeWindow;
   if (w && !w.isNull)
      return w;
   var path = getEnvironmentVariable("GAIA_DEPTH_INPUT");
   if (!path)
      throw new Error("no active window and GAIA_DEPTH_INPUT not set");
   var arr = ImageWindow.open(path);
   if (!arr || arr.length == 0)
      throw new Error("failed to open " + path);
   return arr[0];
}

function main() {
   var result = gradeWindow(resolveTarget());
   var outPath = getEnvironmentVariable("GAIA_DEPTH_OUTPUT");
   if (outPath && result) {
      if (!result.saveAs(outPath, false, false, false, false))
         throw new Error("failed to save output: " + outPath);
      console.noteln("wrote " + outPath);
   }
}

main();
