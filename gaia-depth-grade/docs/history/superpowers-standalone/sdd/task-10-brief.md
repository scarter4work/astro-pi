### Task 10: PJSR harness wrapper

**Files:**
- Create: `pjsr/gaia_depth_grade.js`

**Interfaces:**
- Consumes: the installed `gaia_depth_grade` CLI (`python -m gaia_depth_grade.cli grade <in> <out>`); PI processes ImageSolver and StarXTerminator.
- Produces: a graded XISF on disk. This task is validated manually in the PI harness (GPU + StarXTerminator are not in CI), so it has no pytest step; its "test" is the documented harness run.

- [ ] **Step 1: Write the PJSR wrapper**

`pjsr/gaia_depth_grade.js`:
```javascript
// Gaia Depth Grade — thin PJSR harness wrapper.
// Orchestration only: solve, split stars, hand pixels to the Python core, recombine.
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

   // 6. Recombine (screen) modulated stars over starless.
   var PM = new PixelMath;
   PM.expression = "combine(" + starless.id + ", " + graded.mainView.id + ", op_screen)";
   PM.createNewImage = true;
   PM.newImageId = view.id + "_depthgraded";
   PM.executeOn(starless);
}

gradeWindow(ImageWindow.activeWindow.mainView);
```

- [ ] **Step 2: Document the manual harness run**

Add to `pjsr/gaia_depth_grade.js` header comment (and the project README) the invocation, mirroring the NukeX harness pattern:
```bash
xvfb-run -a /opt/PixInsight/bin/PixInsight.sh --automation-mode \
  --run=/home/scarter4work/projects/gaia-depth-grade/pjsr/gaia_depth_grade.js
```

- [ ] **Step 3: Commit**

```bash
git add pjsr/gaia_depth_grade.js
git commit -m "feat: PJSR harness wrapper (solve, SXT split, python grade, recombine)"
```

---

## Self-Review

**Spec coverage:**
- §3 hybrid architecture → Tasks 9 (CLI/FITS boundary) + 10 (PJSR). ✓
- §4 modules: wcs(T2), detect(T3), distances+DistanceSource(T4), match(T5), transform(T6), modulate(T7), render(T8), cli(T9), config(T1). ✓
- §5 data flow → T9 `grade_array` chains the modules in spec order. ✓
- §6 modulation model → T7 formulas + T8 application. ✓
- §7 loud errors: missing WCS (T2), Gaia failure/empty (T4), stale-cache log (T4), low match rate (T9), honesty tag (T9). ✓
- §8 testing: unit (T2,T3,T6,T7,T8), component-distances against injected source (T4), synthetic E2E (T9), manual PI (T10). ✓
- §9 inputs/layout → file structure + T1 package. Configurable input path handled via CLI `input` arg + default documented in README (note: wire the `~/projects/processing/master` default into the PJSR/README, not the Python CLI, which takes an explicit path). ✓
- §10 data sources → Gaia/Bailer-Jones (T4), plate-solve + SXT (T10). ✓
- Phase 2 (dust) is intentionally out of this plan; `DistanceSource` ABC (T4) is the seam it will extend. ✓

**Placeholder scan:** No TBD/TODO; every code step has complete code. ✓

**Type consistency:** `FieldFootprint` (T2) consumed by T4/T9; detection columns `x,y,flux,fwhm` produced T3, consumed T5/T8; `MatchStats` fields used in T9 qa dict; `Modulation` fields (brightness/size/contrast/saturation) produced T7, consumed T8; `DistanceSource.distances_for` signature consistent T4→T9 (FakeSource). Honesty string identical in T9 impl and T9 test. ✓
