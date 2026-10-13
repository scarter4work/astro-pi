// Gaia Depth Grade — thin PJSR harness wrapper.
// Orchestration only: solve, split stars, hand pixels to the Python core, recombine.
//
// Interactive: open a master and run this script on the active window.
//
// Headless: pass the image via env and launch PixInsight through the repo's
// tools/pi-headless.sh (private Xvfb, never the desktop) in a test instance slot.
// PI's console does not reach stdout; detect completion via the output file.
//   . tools/pi-headless.sh
//   GAIA_DEPTH_INPUT=/path/master.xisf GAIA_DEPTH_OUTPUT=/path/out.xisf \
//     pi_headless /opt/PixInsight/bin/PixInsight.sh -n=94 --automation-mode \
//       --no-startup-scripts -r=/opt/PixInsight/src/scripts/GaiaDepthGrade/gaia_depth_grade.js --force-exit
// The sidecar must be installed (package "Gaia Depth Grade sidecar"); see
// doc/scripts/GaiaDepthGrade/GaiaDepthGrade.html.
//
// Shared helpers (solver preamble, runSidecar, splitStars, writeWcsKeywords, solveAndSplit):
#script-id     GaiaDepthGradeHeadless
#include "gaia_depth_grade_lib.jsh"

function gradeWindow(window) {
   sidecarBin();   // a missing or wrong sidecar fails here, before solving
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

   // 4. Run the Python depth grade (online Gaia query). The config keeps the
   //    query cache under TMP_DIR; the sidecar's default is relative to its
   //    working directory.
   var cfgPath = TMP_DIR + "/grade.toml";
   File.writeTextFile(cfgPath, "cache_dir = \"" + GAIA_CACHE_DIR + "\"\n");
   runSidecar(["grade", inPath, outPath, "--config", cfgPath]);

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
