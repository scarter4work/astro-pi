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
//       --run=/abs/path/pi/gaia_depth_grade.js
//
// Shared helpers (solver preamble, run, splitStars, writeWcsKeywords, solveAndSplit):
#script-id     GaiaDepthGradeHeadless
#include "gaia_depth_grade_lib.jsh"

function gradeWindow(window) {
   gdgEnsureDir(TMP_DIR);
   var view = window.mainView;

   // 1+2. Plate-solve, split stars/starless, stamp WCS onto the stars layer.
   var split = solveAndSplit(window);
   var starsWin = split.stars;
   var starless = split.starless;

   // 3. Export stars layer (+WCS) to FITS for the Python core.
   var inPath  = TMP_DIR + "/stars_in.fits";
   var outPath = TMP_DIR + "/stars_out.fits";
   if (!starsWin.saveAs(inPath, false, false, false, false))
      throw new Error("failed to save stars FITS: " + inPath);

   // 4. Run the Python depth grade.
   run(sidecarCmd(["grade", gdgQuote(inPath), gdgQuote(outPath)]));

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
