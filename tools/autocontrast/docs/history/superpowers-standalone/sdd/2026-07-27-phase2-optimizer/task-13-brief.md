### Task 13: PJSR driver and the PixInsight executor

**Files:**
- Create: `pixinsight/autocontrast_optimize.js`

**Interfaces:**
- Consumes: sidecar ops `analyze` (to resolve the reference), `optimize_begin`, `optimize_step`.
- Produces: `Script > AutoContrast > Optimize`; writes `STATUS_FILE` and `recipe.json`.

Follow the established conventions in `pixinsight/autocontrast_analyze.js`: `envOr()` for every path, `STATUS_FILE` reporting (headless PI writes nothing useful to stdout), `ExternalProcess.execute` for the sidecar.

- [ ] **Step 1: Write the script**

```javascript
/*
 * AutoContrast — Optimize (design §6, §7).
 *
 * Drives the deterministic optimizer: resolve a professional reference for this
 * field, then run the guardrailed beam search toward it. If the image is already
 * good, this DECLINES and leaves it alone — that is a success, not a failure.
 *
 * Phase 2 has no AI. Action ranking is heuristic (§6.2); the VLM director is
 * Phase 3 and must earn its place there.
 *
 * Headless: PixInsight.sh -n --automation-mode \
 *              -r=pixinsight/autocontrast_optimize.js --force-exit
 */

#feature-id    AutoContrast > Optimize

function envOr(name, fallback) {
   var v = getEnvironmentVariable(name);
   return (v && v.length > 0) ? v : fallback;
}

var PROJECT     = envOr("AUTOCONTRAST_PROJECT", "/home/scarter4work/projects/autocontrast");
var PYTHON      = envOr("AUTOCONTRAST_PYTHON",   PROJECT + "/.venv/bin/python");
var INDEX       = envOr("AUTOCONTRAST_INDEX",    PROJECT + "/data/gallery_index.sqlite");
var STORE       = envOr("AUTOCONTRAST_STORE",    PROJECT + "/data/fingerprints.sqlite");
var CACHE_DIR   = envOr("AUTOCONTRAST_CACHE",    PROJECT + "/data/discovery_cache");
var WORK        = envOr("AUTOCONTRAST_WORK",     "/tmp/autocontrast_optimize");
var STATUS_FILE = WORK + "/status.txt";
var RECIPE_FILE = WORK + "/recipe.json";
var MAX_ITER    = parseInt(envOr("AUTOCONTRAST_MAX_ITER", "20"), 10);

var DEFAULT_IMAGE = envOr("AUTOCONTRAST_IMAGE",
   "/mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg");

function writeText(path, text) {
   var f = new File;
   f.createForWriting(path);
   f.write(ByteArray.stringToUTF8(text));
   f.close();
}

function readText(path) {
   return File.readFile(path).utf8ToString();
}

function runSidecar(request) {
   var reqPath  = WORK + "/request.json";
   var respPath = WORK + "/response.json";
   writeText(reqPath, JSON.stringify(request));
   var code = ExternalProcess.execute(PYTHON,
                 ["-m", "autocontrast.sidecar", reqPath, respPath]);
   if (code != 0)
      console.warningln("sidecar exit code = " + code);
   var resp = JSON.parse(readText(respPath));
   if (!resp.ok)
      throw new Error(resp.error);
   return resp.result;
}

function targetImagePath() {
   var w = ImageWindow.activeWindow;
   if (!w.isNull) {
      if (w.filePath && w.filePath.length > 0)
         return w.filePath;
      throw new Error("The active window '" + w.mainView.id +
                      "' has never been saved, so there is no file to optimize.");
   }
   if (!File.exists(DEFAULT_IMAGE))
      throw new Error("No image window is open and DEFAULT_IMAGE does not exist: " +
                      DEFAULT_IMAGE);
   return DEFAULT_IMAGE;
}

function round(x, places) {
   if (x === null || x === undefined) return "n/a";
   var f = Math.pow(10, places);
   return Math.round(x * f) / f;
}

function main() {
   if (!File.directoryExists(WORK))
      File.createDirectory(WORK, true);

   var image = targetImagePath();
   console.writeln("<b>AutoContrast — Optimize</b>");
   console.writeln("image: " + image);
   console.flush();

   /* Reuse the proven §6 miss path to resolve a reference for this field. */
   var analysis = runSidecar({
      op: "analyze", image: image, index: INDEX, store: STORE, cache_dir: CACHE_DIR
   });

   if (analysis.reference === null)
      throw new Error("No usable reference for this field. " + analysis.detail);

   console.writeln("reference: " + analysis.reference.id +
                   "   (" + analysis.reference.license + ")");
   console.writeln("baseline D = " + round(analysis.distance, 4));
   console.flush();

   var started = runSidecar({
      op: "optimize_begin", image: image, work_dir: WORK,
      reference_id: analysis.reference.id,
      reference_fingerprint: analysis.reference_fingerprint,
      pixel_scale_arcsec: analysis.wcs.pixel_scale_arcsec,
      psf_fwhm_arcsec: 2.0,
      palette_class: analysis.palette_class
   });

   var result = null;
   for (var i = 0; i < MAX_ITER; ++i) {
      result = runSidecar({
         op: "optimize_step", work_dir: WORK, session_id: started.session_id
      });
      console.writeln("  iter " + result.iteration +
                      "   D = " + round(result.distance, 4) +
                      (result.converged ? "   [converged]" : ""));
      console.flush();
      if (result.converged) break;
   }

   console.writeln("");
   console.writeln("stopped because: " + result.convergence_reason);

   /* Every candidate a guardrail killed, and why (§12 — degraded paths surface). */
   if (result.guardrail_log && result.guardrail_log.length > 0) {
      console.writeln("");
      console.writeln("<b>Candidates discarded by guardrails</b>");
      for (var j = 0; j < result.guardrail_log.length; ++j) {
         var g = result.guardrail_log[j];
         console.writeln("  iter " + g.iteration + "  " + g.action +
                         " — " + g.failed.join(", ") + ": " + g.reason);
      }
   }

   writeText(RECIPE_FILE, JSON.stringify(result.pixinsight_steps, null, 2));

   console.writeln("");
   if (!result.improved) {
      console.noteln("  DECLINED — this image is already close to the reference.");
      console.writeln("  baseline D = " + round(result.baseline_distance, 4) +
                      "; nothing beat it by the required margin.");
      console.writeln("  Your file was NOT modified.");
      return { ok: true, improved: false, distance: result.baseline_distance };
   }

   console.noteln("  IMPROVED  D " + round(result.baseline_distance, 4) +
                  " -> " + round(result.distance, 4));
   console.writeln("  recipe (" + result.pixinsight_steps.length +
                   " stock processes) written to " + RECIPE_FILE);
   for (var k = 0; k < result.pixinsight_steps.length; ++k)
      console.writeln("    " + (k + 1) + ". " + result.pixinsight_steps[k].process +
                      "  " + result.pixinsight_steps[k].action);

   return { ok: true, improved: true, distance: result.distance };
}

try {
   var out = main();
   writeText(STATUS_FILE,
             "PASS improved=" + out.improved +
             " distance=" + out.distance + "\n");
   console.noteln("<b>AutoContrast Optimize complete</b>");
} catch (e) {
   writeText(STATUS_FILE, "FAIL " + e.message + "\n");
   console.criticalln("AutoContrast Optimize FAILED: " + e.message);
}
```

- [ ] **Step 2: Add `reference_fingerprint` to the analyze response**

The script above needs the reference's fingerprint dict, which `_op_analyze` does not currently return. Add it in `src/autocontrast/sidecar.py` inside `_op_analyze`, right after `result["reference"] = {...}`:

```python
        result["reference_fingerprint"] = best.record.fingerprint.to_dict()
```

- [ ] **Step 3: Verify the sidecar change did not break analyze**

Run: `.venv/bin/python -m pytest tests/test_sidecar.py tests/test_optimize_sidecar.py -v`
Expected: PASS

- [ ] **Step 4: Run the script headlessly against a real print**

```bash
mkdir -p /tmp/autocontrast_optimize
AUTOCONTRAST_IMAGE=/mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg \
/opt/PixInsight/bin/PixInsight.sh -n --automation-mode \
  -r=pixinsight/autocontrast_optimize.js --force-exit
cat /tmp/autocontrast_optimize/status.txt
```

Expected: `PASS improved=false ...` — a finished print should be declined. If it reports `improved=true`, that is a **real finding**, not a test to adjust: record the recipe it proposed and investigate before touching any threshold.

- [ ] **Step 5: Commit**

```bash
git add pixinsight/autocontrast_optimize.js src/autocontrast/sidecar.py
git commit -m "pixinsight: AutoContrast > Optimize driver

Resolves a reference via the proven analyze path, then drives the beam
search. Reports every guardrail-discarded candidate with its reason, and
says plainly when it DECLINES -- which on an already-good image is the
correct outcome, not a failure."
```

---

