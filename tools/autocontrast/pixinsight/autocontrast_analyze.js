/*
 * AutoContrast — Analyze (design §3.1, §6 miss path).
 *
 * Point it at an image and it answers: where is this on the sky, what palette is it,
 * which professional render is the best style target for it, may we legally use that
 * render, and how far is this image from it right now?
 *
 * It changes NOTHING. Phase 1 measures; Phase 2 is the optimizer that will act on the
 * number this prints. Running it on your masters is safe.
 *
 * PixInsight drives the Python sidecar over the file protocol (§3.1): write a request
 * JSON, run `<venv>/bin/python -m autocontrast.sidecar req.json resp.json`, read the
 * response. No network dependency in PJSR, no blocking of the PI event loop.
 *
 * Interactive:  Script > AutoContrast > Analyze   (uses the ACTIVE image window)
 * Headless:     PixInsight.sh -n --automation-mode \
 *                  -r=pixinsight/autocontrast_analyze.js --force-exit
 *               (falls back to DEFAULT_IMAGE when no window is open)
 *
 * A first run on a new field may take a minute: it downloads one press-release render
 * at one request per second. Later runs reuse the cache and are immediate.
 */

#feature-id    AutoContrast > Analyze

/* Every path may be overridden by an environment variable, so the same script serves
 * a different project layout, a scratch store, or a CI run without being edited. */
function envOr(name, fallback) {
   var v = getEnvironmentVariable(name);
   return (v && v.length > 0) ? v : fallback;
}

var PROJECT       = envOr("AUTOCONTRAST_PROJECT", "/home/scarter4work/projects/autocontrast");
var PYTHON        = envOr("AUTOCONTRAST_PYTHON",   PROJECT + "/.venv/bin/python");
var INDEX         = envOr("AUTOCONTRAST_INDEX",    PROJECT + "/data/gallery_index.sqlite");
var STORE         = envOr("AUTOCONTRAST_STORE",    PROJECT + "/data/fingerprints.sqlite");
var CACHE_DIR     = envOr("AUTOCONTRAST_CACHE",    PROJECT + "/data/discovery_cache");
var WORK          = envOr("AUTOCONTRAST_WORK",     "/tmp/autocontrast_analyze");
var STATUS_FILE   = WORK + "/status.txt";

/* Used only when no image window is open (headless runs). */
var DEFAULT_IMAGE = envOr("AUTOCONTRAST_IMAGE",
   "/mnt/qnap/astro_data/9_16_2023/M42/Stacked14_M42_10.0s_Bin1_HaO3_20230916-043848.fit");

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
   return JSON.parse(readText(respPath));
}

/* The sidecar reads pixels from the FILE, not from PI's in-memory view, so an
 * unsaved window has nothing to analyze. Say so plainly rather than silently
 * analyzing some other file. */
function targetImagePath() {
   var w = ImageWindow.activeWindow;
   if (!w.isNull) {
      if (w.filePath && w.filePath.length > 0)
         return w.filePath;
      throw new Error("The active window '" + w.mainView.id +
                      "' has never been saved, so there is no file to analyze. " +
                      "Save it first, or close it to use the configured default.");
   }
   if (!File.exists(DEFAULT_IMAGE))
      throw new Error("No image window is open and DEFAULT_IMAGE does not exist: " +
                      DEFAULT_IMAGE);
   return DEFAULT_IMAGE;
}

function round(x, places) {
   if (x === null || x === undefined)
      return "n/a";
   var f = Math.pow(10, places);
   return Math.round(x * f) / f;
}

function main() {
   if (!File.directoryExists(WORK))
      File.createDirectory(WORK, true);

   var image = targetImagePath();
   console.writeln("<b>AutoContrast — Analyze</b>");
   console.writeln("image: " + image);
   console.flush();

   var resp = runSidecar({
      op: "analyze",
      image: image,
      index: INDEX,
      store: STORE,
      cache_dir: CACHE_DIR
   });

   if (!resp.ok)
      throw new Error(resp.error);

   var r = resp.result;

   console.writeln("");
   console.writeln("<b>Solved position</b>");
   console.writeln("  RA / Dec      : " + round(r.wcs.ra_deg, 4) + "  " +
                                          round(r.wcs.dec_deg, 4) + "   (deg)");
   console.writeln("  field radius  : " + round(r.wcs.fov_radius_arcmin, 2) + "'");
   console.writeln("  pixel scale   : " + round(r.wcs.pixel_scale_arcsec, 4) + "\"/px");
   console.writeln("  WCS source    : " + r.wcs.wcs_source);
   console.writeln("  palette       : " + r.palette_class);

   /* Every candidate that was rejected, and why (§12 — degraded paths surface). */
   if (r.considered && r.considered.length > 0) {
      console.writeln("");
      console.writeln("<b>Candidates considered</b>");
      for (var i = 0; i < r.considered.length; ++i) {
         var c = r.considered[i];
         console.writeln("  " + (c.accepted ? "ACCEPTED " : "rejected ") +
                         c.id + " — " + c.reason);
      }
   }

   if (r.reference === null) {
      console.warningln("");
      console.warningln("No usable reference for this field. " + r.detail);
      return { ok: false, detail: r.detail };
   }

   console.writeln("");
   console.writeln("<b>Reference</b>" + (r.discovered ? "  (discovered this run)" : ""));
   console.writeln("  id            : " + r.reference.id);
   console.writeln("  palette       : " + r.reference.palette_class);
   console.writeln("  separation    : " + round(r.reference.separation_arcmin, 2) + "'");
   console.writeln("  source        : " + r.reference.source_url);
   console.writeln("  license       : " + r.reference.license);
   console.writeln("  attribution   : " + r.reference.attribution);

   console.writeln("");
   console.noteln("  DISTANCE D = " + round(r.distance, 4));

   /* §2.3 flags rather than filters: a palette mismatch does not discard the
    * reference, it narrows what the reference is allowed to say. */
   if (!r.reference.palette_compatible)
      console.warningln("  palette mismatch (" + r.palette_class + " vs " +
                        r.reference.palette_class + ") — chroma is gated OUT; " +
                        "structure and tone only (§2.3).");
   else
      console.writeln("  palette compatible — chroma guidance is in play.");

   console.writeln("");
   console.writeln("Phase 1 measures only. Nothing was modified.");

   return { ok: true, distance: r.distance, reference: r.reference.id,
            discovered: r.discovered };
}

try {
   var out = main();
   writeText(STATUS_FILE,
             (out.ok ? "PASS" : "NOREF") +
             " distance=" + (out.distance === undefined ? "n/a" : out.distance) +
             " reference=" + (out.reference || "none") +
             " discovered=" + (out.discovered === undefined ? "n/a" : out.discovered) +
             "\n");
   console.noteln("<b>AutoContrast Analyze complete</b>");
} catch (e) {
   writeText(STATUS_FILE, "FAIL " + e.message + "\n");
   console.criticalln("AutoContrast Analyze FAILED: " + e.message);
}
