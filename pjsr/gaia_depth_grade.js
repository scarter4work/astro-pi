// Gaia Depth Grade — thin PJSR harness wrapper.
// Orchestration only: solve, split stars, hand pixels to the Python core, recombine.
//
// Run in headless mode inside the PixInsight harness:
//   xvfb-run -a /opt/PixInsight/bin/PixInsight.sh --automation-mode \
//     --run=/home/scarter4work/projects/gaia-depth-grade/pjsr/gaia_depth_grade.js
//
#include <pjsr/StdButton.jsh>

#define PY_BIN   "/home/scarter4work/projects/gaia-depth-grade/.venv/bin/python"
#define PKG      "gaia_depth_grade.cli"
#define TMP_DIR  "/tmp/gaia_depth_grade"

function run(cmd) {
   var p = new ExternalProcess(cmd);
   p.waitForFinished();
   if (p.exitStatus != ProcessExitStatus_NormalExit || p.exitCode != 0)
      throw new Error("command failed: " + cmd + "\n" + p.stderr);
}

function gradeWindow(view) {
   File.createDirectory(TMP_DIR, true);

   // 1. Plate-solve (writes WCS keywords into the view).
   var solver = new ImageSolver;
   solver.SolveImage(view);

   // 2. Split stars/starless with StarXTerminator.
   var sxt = new StarXTerminator;
   sxt.unscreen = true;          // produce a linear stars image
   sxt.executeOn(view);          // creates a "<id>_stars" window per SXT settings
   var stars = View.viewById(view.id + "_stars");
   var starless = view;          // SXT leaves starless in place

   // 3. Export stars layer (+WCS) to FITS for the Python core.
   var inPath  = TMP_DIR + "/stars_in.fits";
   var outPath = TMP_DIR + "/stars_out.fits";
   stars.window.saveAs(inPath, false, false, false, false);

   // 4. Run the Python depth grade.
   run([PY_BIN, "-m", PKG, "grade", inPath, outPath]);

   // 5. Read the modulated stars layer back.
   var graded = ImageWindow.open(outPath)[0];

   // 6. Recombine modulated stars over starless via screen blend.
   //    Screen: result = 1 - (1-A)*(1-B), written in PixelMath as ~((~A)*(~B)).
   //    This is the standard way to add a StarXTerminator stars layer back.
   var PM = new PixelMath;
   PM.expression = "~((~" + starless.id + ") * (~" + graded.mainView.id + "))";
   PM.createNewImage = true;
   PM.newImageId = view.id + "_depthgraded";
   PM.executeOn(starless);
}

gradeWindow(ImageWindow.activeWindow.mainView);
