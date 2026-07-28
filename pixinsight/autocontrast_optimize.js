/*
 * AutoContrast — Optimize (design §2.5, §6, §7, §12).
 *
 * Drives the deterministic optimizer: resolve a professional reference for this
 * field, then run the guardrailed beam search toward it. If the image is already
 * good, this DECLINES and leaves it alone — that is a success, not a failure.
 *
 * Phase 2 has no AI. Action ranking is heuristic (§6.2); the VLM director is
 * Phase 3 and must earn its place there.
 *
 * THIS SCRIPT IS THE PIXINSIGHT EXECUTOR. There is no Python executor for
 * production. §2.5's protocol is batched precisely so that PixInsight — not a
 * numpy approximation — applies every candidate pixel:
 *
 *     optimize_begin  ──────►  sidecar: measure the baseline, plan batch 1
 *                     ◄──────  {session_id, instructions, converged, ...}
 *     WE apply N stock processes here and save N candidates
 *     optimize_step   ──────►  sidecar: measure all N, guardrail, score, prune
 *                     ◄──────  the next batch  |  or  converged + recipe
 *
 * §2.2 is why: production never optimizes against an approximation. That is
 * also why candidates are 16-bit TIFF rather than the sidecar's default 8-bit
 * PNG — see CANDIDATE_SUFFIX.
 *
 * Interactive:  Script > AutoContrast > Optimize   (uses the ACTIVE image window)
 * Headless:     PixInsight.sh -n --automation-mode \
 *                  -r=pixinsight/autocontrast_optimize.js --force-exit
 *               (falls back to DEFAULT_IMAGE when no window is open)
 *
 * Nothing this script does modifies your file. It writes candidates and a
 * recipe under WORK and reports; applying the recipe to the full-resolution
 * master is a separate, deliberate step.
 */

#feature-id    AutoContrast > Optimize

/* Every path may be overridden by an environment variable, so the same script serves
 * a different project layout, a scratch store, or a CI run without being edited. */
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
/* §12 requires degraded paths to SURFACE, and headless PixInsight writes
 * nothing from `console` to stdout — measured: a full run's report vanished and
 * only StarXTerminator's SIGSEGV backtraces (which go to stderr from the C++
 * side) came through. So every line this script prints is mirrored here.
 * Without it, the guardrail reasons and the `noted` entries Tasks 3 and 10 built
 * would exist only in an interactive session, which is the one place a batch run
 * never is. */
var REPORT_FILE = WORK + "/report.txt";

/* Used only when no image window is open (headless runs). */
var DEFAULT_IMAGE = envOr("AUTOCONTRAST_IMAGE",
   "/mnt/qnap/astro_data/prints/M42_hoo_2_18_23.jpg");

/* The sidecar's own default `max_dim`. Passed EXPLICITLY on `optimize_begin` and
 * used to build the proxy, so the two ends cannot drift: the proxy is exactly
 * the array the sidecar will fingerprint, and `downsample_factor` on it is 1.0.
 * Not a §3.7 search tunable — it is the resolution the whole design already
 * searches at (see BeamConfig's cost table, "the real 1600px search proxy"). */
var MAX_DIM = parseInt(envOr("AUTOCONTRAST_MAX_DIM", "1600"), 10);

/* §7's psf_fwhm_arcsec, the band limit below which no action may be proposed
 * (§2.2/§4.4). The sidecar's `analyze` default; stated here because
 * `optimize_begin` requires it explicitly. */
var PSF_FWHM_ARCSEC = parseFloat(envOr("AUTOCONTRAST_PSF_FWHM", "2.0"));

/* 16-bit TIFF, NOT the sidecar's default 8-bit `.png`.
 *
 * Every candidate makes a full round trip through disk: PixInsight writes it,
 * the sidecar reads it back to score it, and the survivors become the parents
 * of the next iteration. At 8 bits that quantizes the search itself, which is
 * exactly the approximation §2.2 forbids production from optimizing against.
 *
 * Measured, not assumed: Pillow opens a 48-bit RGB TIFF WITHOUT ERROR and hands
 * back 8 bits, so this suffix only became safe once `io/loaders.py` grew a
 * `tifffile` decode for it. `optimize_begin` validates the suffix against
 * `loaders.supported_suffixes()`, so a sidecar without that decode will refuse
 * this run loudly rather than silently truncate it. */
var CANDIDATE_SUFFIX = envOr("AUTOCONTRAST_CANDIDATE_SUFFIX", ".tif");
var CANDIDATE_BITS   = 16;

/* §5.2 tier 4: a manual annotation, "a first-class fallback, not an error".
 *
 * Needed more often than it sounds. A finished JPEG print has no FITS header
 * (tier 1) and typically no readable AVM tag (tier 2), and PixInsight's
 * ImageSolver could not solve the test print even given its true centre and
 * plate scale (tier 3) — so for a print, tier 4 is frequently the only tier
 * left. All four values are required together, because a position without a
 * pixel scale would leave §4.4's band limiting to a guess, and a guessed scale
 * silently changes which actions are even proposable (§2.2).
 *
 * Ignored whenever the file can be solved on its own: `acquire_wcs` consults
 * the tiers in order, so a real WCS in the file always wins over these. */
function manualAnnotation() {
   var ra    = envOr("AUTOCONTRAST_MANUAL_RA", "");
   var dec   = envOr("AUTOCONTRAST_MANUAL_DEC", "");
   var fov   = envOr("AUTOCONTRAST_MANUAL_FOV_RADIUS", "");
   var scale = envOr("AUTOCONTRAST_MANUAL_PIXEL_SCALE", "");
   var given = [ra, dec, fov, scale];
   var count = 0;
   for (var i = 0; i < given.length; ++i)
      if (given[i].length > 0) ++count;

   if (count === 0)
      return null;
   if (count < given.length)
      throw new Error("a manual annotation needs all four of " +
                      "AUTOCONTRAST_MANUAL_RA, _DEC, _FOV_RADIUS (arcmin) and " +
                      "_PIXEL_SCALE (arcsec/px); only " + count + " were set. " +
                      "A partial annotation would leave the rest to a guess.");

   return {
      ra_deg: parseFloat(ra),
      dec_deg: parseFloat(dec),
      fov_radius_arcmin: parseFloat(fov),
      pixel_scale_arcsec: parseFloat(scale)
   };
}

/* A driver-side stop, not a search parameter: §6.3's convergence conditions and
 * the §3.3 attempt cap are the sidecar's, untouched. This only stops a protocol
 * loop that is not making progress from running forever, and it says so loudly
 * if it ever binds. At iteration_cap 20 and up to 3 batches per iteration the
 * real ceiling is ~60. */
var MAX_BATCHES = 200;

function writeText(path, text) {
   var f = new File;
   f.createForWriting(path);
   f.write(ByteArray.stringToUTF8(text));
   f.close();
}

function readText(path) {
   return File.readFile(path).utf8ToString();
}

/* Everything the script reports goes through these three, never through
 * `console` directly, so the file mirror cannot fall out of step with what an
 * interactive user sees. `<b>`/tags are PI console markup; they are stripped on
 * the way to the file. */
var reportLines = [];

function _record(text) {
   reportLines.push(String(text).replace(/<\/?[a-z]+>/g, ""));
}

function say(text)  { _record(text); console.writeln(text);   console.flush(); }
function warn(text) { _record("! " + text); console.warningln(text); console.flush(); }
function note(text) { _record("* " + text); console.noteln(text);    console.flush(); }

function flushReport(header) {
   try {
      writeText(REPORT_FILE, header + "\n\n" + reportLines.join("\n") + "\n");
   } catch (e) {
      // The report is diagnostics; failing to write it must not mask the result
      // the run actually produced, which STATUS_FILE still carries.
      console.criticalln("could not write " + REPORT_FILE + ": " + e.message);
   }
}

function round(x, places) {
   if (x === null || x === undefined)
      return "n/a";
   var f = Math.pow(10, places);
   return Math.round(x * f) / f;
}

/* ------------------------------------------------------------------------- *
 * Sidecar transport (§3.1) — identical convention to autocontrast_analyze.js.
 * ------------------------------------------------------------------------- */

var sidecarCalls = 0;

function runSidecar(request) {
   ++sidecarCalls;
   var reqPath  = WORK + "/request.json";
   var respPath = WORK + "/response.json";
   writeText(reqPath, JSON.stringify(request));
   var code = ExternalProcess.execute(PYTHON,
                 ["-m", "autocontrast.sidecar", reqPath, respPath]);
   if (code != 0)
      warn("sidecar exit code = " + code);
   var resp = JSON.parse(readText(respPath));
   if (!resp.ok) {
      /* The sidecar's traceback is the only diagnosis available headlessly;
       * dropping it here would leave a one-line error and no way to act on it. */
      if (resp.traceback)
         console.criticalln(resp.traceback);
      throw new Error(resp.error);
   }
   return resp.result;
}

/* The sidecar reads pixels from the FILE, not from PI's in-memory view, so an
 * unsaved window has nothing to optimize. Say so plainly rather than silently
 * optimizing some other file. */
function targetImagePath() {
   var w = ImageWindow.activeWindow;
   if (!w.isNull) {
      if (w.filePath && w.filePath.length > 0)
         return w.filePath;
      throw new Error("The active window '" + w.mainView.id +
                      "' has never been saved, so there is no file to optimize. " +
                      "Save it first, or close it to use the configured default.");
   }
   if (!File.exists(DEFAULT_IMAGE))
      throw new Error("No image window is open and DEFAULT_IMAGE does not exist: " +
                      DEFAULT_IMAGE);
   return DEFAULT_IMAGE;
}

/* ------------------------------------------------------------------------- *
 * The search proxy.
 *
 * The search runs at MAX_DIM, and PixInsight — not the sidecar — must be the
 * one to get there. If the full-resolution master were handed to
 * `optimize_begin`, every instruction's `parent_path` would be that master and
 * PixInsight would process and save ~60 candidates at full size. On the test
 * print (5318x3975) that is 127 MB per 16-bit candidate, ~8 GB for one run,
 * into a WORK directory whose default is /tmp — tmpfs, 16 GB, backed by RAM.
 * At MAX_DIM the same run writes ~15 MB per candidate.
 *
 * This is NOT the "pre-corrected pixel scale" mistake. The effective scale the
 * fingerprint is taken at is identical either way:
 *
 *     master  : native_scale x downsample_factor(master, load(master)) = native x 3.32
 *     proxy   : proxy_scale  x downsample_factor(proxy,  load(proxy))  = (native x 3.32) x 1.0
 *
 * `optimize_begin` multiplies by `downsample_factor` of whatever file it is
 * given, and the proxy IS the file it is given, so what it must be told is the
 * proxy's own native on-sky scale. The master's path still travels as
 * `source_path`, which is what the recipe is ultimately meant to be applied to.
 * ------------------------------------------------------------------------- */

function buildProxy(sourcePath, nativePixelScaleArcsec) {
   var w = ImageWindow.open(sourcePath)[0];
   if (w.isNull)
      throw new Error("PixInsight could not open " + sourcePath);

   var nativeW = w.mainView.image.width;
   var nativeH = w.mainView.image.height;
   var longest = Math.max(nativeW, nativeH);

   /* Never upscale — a smaller master is already its own proxy (the sidecar's
    * `max_dim` has the same "never upscale" rule, and the two must agree). */
   var factor = (longest > MAX_DIM) ? (MAX_DIM / longest) : 1.0;
   var targetW = Math.max(1, Math.round(nativeW * factor));
   var targetH = Math.max(1, Math.round(nativeH * factor));

   if (factor < 1.0) {
      var R = new Resample;
      R.xSize = targetW;
      R.ySize = targetH;
      R.mode = Resample.prototype.AbsolutePixels;
      /* ForceWidthAndHeight with BOTH sizes computed from the master's own
       * aspect. Forcing a square MAX_DIM x MAX_DIM here would stretch the
       * image, and every structural metric in §4 would then be measuring the
       * distortion instead of the picture. */
      R.absoluteMode = Resample.prototype.ForceWidthAndHeight;
      R.interpolation = Resample.prototype.Auto;
      if (!R.executeOn(w.mainView)) {
         w.forceClose();
         throw new Error("Resample failed while building the " + MAX_DIM +
                         "px search proxy for " + sourcePath);
      }
   }

   w.setSampleFormat(CANDIDATE_BITS, false);
   var proxyPath = WORK + "/proxy" + CANDIDATE_SUFFIX;
   var saved = w.saveAs(proxyPath, false, false, false, false);
   var actualW = w.mainView.image.width;
   var actualH = w.mainView.image.height;
   var bits = w.bitsPerSample;
   var isColor = w.mainView.image.isColor;
   w.forceClose();

   if (!saved)
      throw new Error("could not save the search proxy to " + proxyPath);

   /* The proxy's OWN on-sky scale: each of its pixels covers `longest/actual`
    * master pixels. Exact, because both numbers are read off the images
    * themselves rather than assumed from the requested MAX_DIM. */
   var scale = null;
   if (nativePixelScaleArcsec !== null && nativePixelScaleArcsec !== undefined)
      scale = nativePixelScaleArcsec * (longest / Math.max(actualW, actualH));

   return {
      path: proxyPath,
      width: actualW, height: actualH,
      nativeWidth: nativeW, nativeHeight: nativeH,
      bits: bits, isColor: isColor,
      pixelScaleArcsec: scale
   };
}

/* ------------------------------------------------------------------------- *
 * §6.2's action -> stock PixInsight process mapping, realized.
 *
 * `process` and `params` arrive on the instruction from
 * `Recipe.to_pixinsight_steps()` — the SAME rendering the §12 audit log
 * reports — so what runs here is what the recipe will claim ran. This file
 * never invents a process name and never re-derives one from the action key.
 *
 * Every builder is free to THROW. A throw is not a run-ending error: the
 * instruction is omitted from `produced`, the sidecar logs an executor failure
 * for that candidate alone, and the branch asks for a replacement. That is the
 * designed path for "PixInsight cannot apply this process to this image", and
 * it is why PixInsight's own out-of-range messages are allowed to propagate
 * verbatim rather than being smoothed over with a clamp — a clamped parameter
 * would make the recipe describe something other than what ran.
 * ------------------------------------------------------------------------- */

function buildMultiscaleLinearTransform(params) {
   /* local_contrast: boost exactly one starlet detail layer.
    * `layer` is 0-based and finest-first, which is also PI's `layers` array
    * order (index 0 is layer 1 in the UI). The array carries one entry per
    * layer plus a final residual entry, so a request for layer L needs L+2. */
   var layer = params.layer;
   if (layer === null || layer === undefined)
      throw new Error("local_contrast instruction carries no 'layer' parameter");

   var P = new MultiscaleLinearTransform;
   var layers = [];
   for (var i = 0; i <= layer + 1; ++i) {
      var bias = (i === layer) ? params.strength : 0.0;
      // enabled, biasEnabled, bias, noiseReductionEnabled, threshold, amount, iterations
      layers.push([true, true, bias, false, 3.0, 1.0, 1]);
   }
   P.layers = layers;
   P.transform = MultiscaleLinearTransform.prototype.StarletTransform;
   return P;
}

function buildLocalHistogramEqualization(params, proxyPixelScaleArcsec) {
   /* local_equalize: equalize over a kernel of the action's angular scale.
    * The action is specified in ARCSECONDS; PI wants PIXELS, so the proxy's own
    * scale is the conversion — using the master's would ask for a kernel 3x too
    * small. PixInsight enforces radius in [16, 512] and RAISES outside it; that
    * raise is deliberately not caught here (see the section comment). */
   var radiusArcsec = params.radius_arcsec;
   if (radiusArcsec === null || radiusArcsec === undefined)
      throw new Error("local_equalize instruction carries no 'radius_arcsec' parameter");
   if (!proxyPixelScaleArcsec)
      throw new Error("local_equalize needs a pixel scale to convert " +
                      radiusArcsec + "\" to pixels, and this image has no WCS scale");

   var radiusPx = Math.round(radiusArcsec / proxyPixelScaleArcsec);
   var P = new LocalHistogramEqualization;
   P.radius = radiusPx;   // throws, loudly and with the number, if out of range
   P.amount = params.strength;
   P.slopeLimit = 2.0;
   P.histogramBins = LocalHistogramEqualization.prototype.Bit8;
   P.circularKernel = true;
   return P;
}

function buildHDRMultiscaleTransform(params) {
   /* core_hdr: compress the bright cores over N wavelet layers.
    * The magnitude IS the layer count — §6.2 discretizes this action as
    * {gentle: 2, moderate: 3, strong: 4} layers, so `strength` carries no extra
    * information here and inventing a second knob for it (overdrive, say) would
    * be a free-form parameter, which §6.2 forbids outright. */
   var layers = params.layers;
   if (layers === null || layers === undefined)
      throw new Error("core_hdr instruction carries no 'layers' parameter");

   var P = new HDRMultiscaleTransform;
   P.numberOfLayers = layers;
   P.numberOfIterations = 1;
   P.medianTransform = true;
   P.toLightness = true;
   return P;
}

function buildCurvesTransformation(params) {
   /* tonal_reshape: a MONOTONE S-curve on RGB/K (§6.2's `monotone: true`).
    * Monotonicity is structural, not checked after the fact: the two interior
    * anchors move by at most 0.15 * strength, and at the largest strength in
    * the menu (0.85) that is 0.1275 — so the shadow anchor stays above 0.12 and
    * the highlight anchor below 0.88, and the four control points remain
    * strictly increasing in both coordinates. */
   var s = params.strength;
   var lift = 0.15 * s;
   var P = new CurvesTransformation;
   P.K = [[0.0, 0.0], [0.25, 0.25 - lift], [0.75, 0.75 + lift], [1.0, 1.0]];
   P.Kt = CurvesTransformation.prototype.AkimaSubsplines;
   return P;
}

function buildHistogramTransformation(params, view) {
   /* black_point: raise the black point, CLIP-LIMITED (§6.2's `clip_limited`).
    * The limit is the image's own 1st percentile, measured here rather than
    * assumed, and the move is a FRACTION of it (strength <= 0.85 < 1), so the
    * black point provably never reaches the 1st percentile and no more than 1%
    * of pixels can be clipped by construction. §7's shadow-clipping guardrail
    * is the independent check; this is the action refusing to need it. */
   if (params.clip_limited !== true)
      throw new Error("black_point instruction is not clip-limited; refusing to " +
                      "raise a black point with no bound on what it clips");

   var h = new Histogram(view.image);
   var onePercent = h.normalizedClipLow(Math.round(0.01 * h.totalCount));
   var shadows = onePercent * params.strength;

   var P = new HistogramTransformation;
   // rows: R, G, B, RGB/K, Alpha;  each [shadows, midtones, highlights, low, high]
   P.H = [[0.0, 0.5, 1.0, 0.0, 1.0],
          [0.0, 0.5, 1.0, 0.0, 1.0],
          [0.0, 0.5, 1.0, 0.0, 1.0],
          [shadows, 0.5, 1.0, 0.0, 1.0],
          [0.0, 0.5, 1.0, 0.0, 1.0]];
   return P;
}

function buildColorSaturation(params) {
   /* chroma: a uniform saturation boost across all hues.
    * Flat rather than hue-selective on purpose — §2.3 gates chroma guidance ON
    * palette compatibility, and a hue-selective curve would be the reference's
    * color cloud pushing specific hues, which is the thing that gate exists to
    * prevent. The proposer only ever emits this action when the gate is open. */
   var s = params.strength;
   var P = new ColorSaturation;
   P.HS = [[0.0, s], [1.0, s]];
   P.HSt = ColorSaturation.prototype.AkimaSubsplines;
   P.hueShift = 0.0;
   return P;
}

function buildBackgroundNeutralization(_params) {
   /* background_neutralize: a mode change, applied at most once per recipe
    * (§6.2's ONCE_ONLY), so it has no magnitude. The target is its own
    * background reference — there is no second view to point at. */
   var P = new BackgroundNeutralization;
   P.backgroundReferenceViewId = "";
   P.backgroundLow = 0.0;
   P.backgroundHigh = 0.1;
   P.mode = BackgroundNeutralization.prototype.RescaleAsNeeded;
   return P;
}

function buildStarXTerminator(_params) {
   /* star_split: separate stars from the nebula so later actions act on
    * structure without inflating stars. Also ONCE_ONLY, also no magnitude.
    * `stars = false` keeps the starless image as the result. */
   var P = new StarXTerminator;
   P.stars = false;
   P.unscreen = false;
   P.overlap = 0.20;
   return P;
}

/* The one place a process name becomes a process. Keyed by the string the
 * recipe carries, so an unmapped process is caught by name here rather than
 * running the wrong thing. */
function buildProcess(processName, params, view, proxyPixelScaleArcsec) {
   switch (processName) {
      case "MultiscaleLinearTransform":
         return buildMultiscaleLinearTransform(params);
      case "LocalHistogramEqualization":
         return buildLocalHistogramEqualization(params, proxyPixelScaleArcsec);
      case "HDRMultiscaleTransform":
         return buildHDRMultiscaleTransform(params);
      case "CurvesTransformation":
         return buildCurvesTransformation(params);
      case "HistogramTransformation":
         return buildHistogramTransformation(params, view);
      case "ColorSaturation":
         return buildColorSaturation(params);
      case "BackgroundNeutralization":
         return buildBackgroundNeutralization(params);
      case "StarXTerminator":
         return buildStarXTerminator(params);
   }
   throw new Error("no PixInsight process is wired for '" + processName +
                   "'; the sidecar's PROCESS_FOR_KIND and this script disagree");
}

/* ------------------------------------------------------------------------- *
 * Executing one instruction.
 *
 * Returns the candidate's path, or null. NULL IS A SUPPORTED OUTCOME: the
 * instruction is then simply left out of `produced`, and the sidecar treats it
 * as an executor failure for that candidate only — logged, run continues. What
 * must never happen is fabricating an output file for a process that did not
 * run, so every failure route below returns null and says why.
 * ------------------------------------------------------------------------- */

var executorFailures = [];

function noteExecutorFailure(inst, reason) {
   executorFailures.push({
      instruction_id: inst.instruction_id,
      action: inst.action_key,
      process: inst.process,
      reason: reason
   });
   warn("    executor: " + inst.action_key + " (" + inst.process +
                     ") — " + reason);
}

function applyInstruction(inst, proxyPixelScaleArcsec) {
   var w = null;
   try {
      var opened = ImageWindow.open(inst.parent_path);
      if (opened.length < 1 || opened[0].isNull)
         throw new Error("could not open parent " + inst.parent_path);
      w = opened[0];

      var P = buildProcess(inst.process, inst.params, w.mainView,
                           proxyPixelScaleArcsec);

      /* `executeOn` returns FALSE on a process that failed without throwing.
       * That is not hypothetical: StarXTerminator SIGSEGVs under
       * --automation-mode on this install, PixInsight catches the signal, and
       * the view comes back byte-identical. Trusting the boolean is what keeps
       * that from being saved as a candidate. */
      if (!P.executeOn(w.mainView)) {
         w.forceClose();
         w = null;
         noteExecutorFailure(inst, "PixInsight reported that " + inst.process +
                             " did not execute (executeOn returned false)");
         return null;
      }

      w.setSampleFormat(CANDIDATE_BITS, false);
      var saved = w.saveAs(inst.candidate_path, false, false, false, false);
      w.forceClose();
      w = null;

      if (!saved) {
         noteExecutorFailure(inst, "could not save the candidate to " +
                             inst.candidate_path);
         return null;
      }
      return inst.candidate_path;
   } catch (e) {
      if (w !== null && !w.isNull) {
         try { w.forceClose(); } catch (ignored) { /* already gone */ }
      }
      noteExecutorFailure(inst, e.message);
      return null;
   }
}

function executeBatch(instructions, proxyPixelScaleArcsec) {
   var produced = [];
   for (var i = 0; i < instructions.length; ++i) {
      var inst = instructions[i];
      var path = applyInstruction(inst, proxyPixelScaleArcsec);
      if (path !== null)
         produced.push({ instruction_id: inst.instruction_id, path: path });
   }
   return produced;
}

/* ------------------------------------------------------------------------- *
 * §12: every degraded path surfaces.
 * ------------------------------------------------------------------------- */

function reportGuardrailLog(log) {
   var discarded = [];
   var noted = [];
   var i, g;
   for (i = 0; i < log.length; ++i) {
      g = log[i];
      if (g.failed && g.failed.length > 0) discarded.push(g);
      if (g.noted && g.noted.length > 0)   noted.push(g);
   }

   say("");
   if (discarded.length === 0) {
      say("<b>Candidates discarded</b>: none.");
   } else {
      say("<b>Candidates discarded</b> (" + discarded.length +
                      ") — a violation discards the candidate, it is never a " +
                      "score term (§7)");
      for (i = 0; i < discarded.length; ++i) {
         g = discarded[i];
         say("  iter " + g.iteration + "  " + (g.action || g.branch) +
                         "  [" + g.failed.join(", ") + "] " + g.reason);
      }
   }

   /* `noted` is NOT a discard. It is a guardrail saying it could not actually
    * protect anything here (star integrity on a starless frame), or a branch
    * saying it stopped searching before it filled its slots (attempt_cap).
    * Either one means the run was less protected, or less thorough, than the
    * headline number suggests — so it gets its own section rather than being
    * folded in with the discards or, worse, left in the JSON unread. */
   say("");
   if (noted.length === 0) {
      say("<b>Noted</b>: none.");
   } else {
      say("<b>Noted</b> (" + noted.length +
                      ") — not discards; degraded or bounded paths (§12)");
      for (i = 0; i < noted.length; ++i) {
         g = noted[i];
         note("  iter " + g.iteration + "  " +
                        (g.action || ("branch " + g.branch)) +
                        "  [" + g.noted.join(", ") + "] " + g.reason);
      }
   }
   return { discarded: discarded.length, noted: noted.length };
}

function reportExecutorFailures() {
   say("");
   if (executorFailures.length === 0) {
      say("<b>PixInsight executor failures</b>: none — every " +
                      "instruction was applied and saved.");
      return;
   }
   warn("<b>PixInsight executor failures</b> (" +
                     executorFailures.length + ") — these instructions were " +
                     "omitted from the results, so the sidecar scored no " +
                     "candidate for them");
   for (var i = 0; i < executorFailures.length; ++i) {
      var f = executorFailures[i];
      warn("  " + f.instruction_id + "  " + f.action +
                        " (" + f.process + ") — " + f.reason);
   }
}

/* ------------------------------------------------------------------------- *
 * main
 * ------------------------------------------------------------------------- */

function main() {
   if (!File.directoryExists(WORK))
      File.createDirectory(WORK, true);

   var totalTime = new ElapsedTime;
   var image = targetImagePath();

   say("<b>AutoContrast — Optimize</b>");
   say("image     : " + image);
   say("work dir  : " + WORK);
   say("candidates: " + CANDIDATE_BITS + "-bit " + CANDIDATE_SUFFIX +
                   "  (§2.2 — an 8-bit intermediate is an approximation, and " +
                   "production does not optimize against one)");

   /* --- 1. Resolve a reference for this field via the proven §6 miss path. -- */
   var manual = manualAnnotation();
   if (manual !== null)
      say("manual    : RA " + manual.ra_deg + "  Dec " + manual.dec_deg +
                      "  radius " + manual.fov_radius_arcmin + "'  scale " +
                      manual.pixel_scale_arcsec + "\"/px  (§5.2 tier 4 — used " +
                      "ONLY if the file carries no WCS of its own)");

   var analysisTime = new ElapsedTime;
   var analysis;
   try {
      analysis = runSidecar({
         op: "analyze", image: image, index: INDEX, store: STORE,
         cache_dir: CACHE_DIR, manual: manual
      });
   } catch (e) {
      /* By far the most common way this call fails, and the one whose default
       * message does not say what to do about it in THIS script's terms. */
      if (manual === null && e.message.indexOf("cannot solve a WCS") >= 0)
         throw new Error(e.message + " For this driver that means setting " +
                         "AUTOCONTRAST_MANUAL_RA, _DEC, _FOV_RADIUS (arcmin) " +
                         "and _PIXEL_SCALE (arcsec/px) — §5.2 tier 4, a " +
                         "first-class fallback. The scale must be measured, " +
                         "not guessed: it sets the band limit that decides " +
                         "which actions may be proposed at all (§2.2, §4.4).");
      throw e;
   }
   say("");
   say("analyze   : " + analysisTime.text);

   if (analysis.considered && analysis.considered.length > 0) {
      for (var i = 0; i < analysis.considered.length; ++i) {
         var c = analysis.considered[i];
         say("  " + (c.accepted ? "ACCEPTED " : "rejected ") +
                         c.id + " — " + c.reason);
      }
   }
   if (analysis.reference === null)
      throw new Error("No usable reference for this field. " + analysis.detail);

   say("reference : " + analysis.reference.id);
   say("  license : " + analysis.reference.license);
   say("  credit  : " + analysis.reference.attribution);
   say("  palette : " + analysis.palette_class + " vs " +
                   analysis.reference.palette_class +
                   (analysis.reference.palette_compatible
                      ? "  — compatible, chroma guidance is in play"
                      : "  — MISMATCH, chroma is gated OUT (§2.3)"));
   say("  scale   : " + round(analysis.wcs.pixel_scale_arcsec, 4) +
                   "\"/px native   (" + analysis.wcs.wcs_source + ")");
   note("  D = " + round(analysis.distance, 4) +
        "   (the MASTER, downsampled by the sidecar)");

   /* --- 2. Build the search proxy PixInsight will actually process. -------- */
   var proxy = buildProxy(image, analysis.wcs.pixel_scale_arcsec);
   say("");
   say("proxy     : " + proxy.nativeWidth + "x" + proxy.nativeHeight +
                   " -> " + proxy.width + "x" + proxy.height +
                   "  " + proxy.bits + "-bit" + (proxy.isColor ? " RGB" : " GRAY"));
   say("  scale   : " + round(proxy.pixelScaleArcsec, 4) +
                   "\"/px  (the proxy's OWN on-sky scale; optimize_begin " +
                   "applies its own downsample correction on top)");

   /* --- 3. Open the session and take instruction batch 1. ------------------ */
   var begun = runSidecar({
      op: "optimize_begin",
      image: proxy.path,          // the proxy IS the image the session optimizes
      source_path: image,         // ...and this is what the recipe is FOR
      work_dir: WORK,
      reference_id: analysis.reference.id,
      reference_fingerprint: analysis.reference_fingerprint,
      pixel_scale_arcsec: proxy.pixelScaleArcsec,
      psf_fwhm_arcsec: PSF_FWHM_ARCSEC,
      palette_class: analysis.palette_class,
      max_dim: MAX_DIM,
      candidate_suffix: CANDIDATE_SUFFIX
   });

   say("");
   say("session   : " + begun.session_id);
   note("  baseline D = " + round(begun.baseline_distance, 4) +
        "   (the PROXY — this is the number the search descends)");
   /* The two D values above are the SAME image measured on two different
    * arrays: the sidecar's Pillow downsample of the master, and PixInsight's
    * Resample. They do not have to agree, and measured on the test print they
    * do not (0.2264 vs 0.2467, +9%) — a resampler changes fine structure, and
    * the fingerprint measures fine structure (§4.2). The search is internally
    * consistent because every candidate is measured against the proxy baseline,
    * but the difference is real and is called out rather than left for someone
    * to trip over. */
   if (Math.abs(begun.baseline_distance - analysis.distance) > 0.01)
      warn("  the two differ by " +
           round(Math.abs(begun.baseline_distance - analysis.distance), 4) +
           " — the proxy's resampler is not the sidecar's, and the fingerprint " +
           "measures fine structure. The search descends the PROXY number; the " +
           "master's own D is not directly comparable to it.");

   /* --- 4. The §2.5 loop: apply, report, step, repeat. --------------------- */
   var instructions = begun.instructions;
   var result = null;
   var batches = 0;
   var candidatesApplied = 0;
   var piTime = 0;

   while (true) {
      ++batches;
      if (batches > MAX_BATCHES)
         throw new Error("the protocol loop ran " + MAX_BATCHES + " batches " +
                         "without converging; stopping rather than looping " +
                         "forever. The session file under " + WORK +
                         " records where it got to.");

      var batchTime = new ElapsedTime;
      var produced = executeBatch(instructions, proxy.pixelScaleArcsec);
      piTime += batchTime.value;
      candidatesApplied += produced.length;

      result = runSidecar({
         op: "optimize_step",
         work_dir: WORK,
         session_id: begun.session_id,
         /* The IDENTICAL string handed to optimize_begin. `loop.resume` compares
          * it to the session's `proxy_path` with exact string equality and
          * refuses a mismatch, because path identity is the only evidence that
          * the resumed image is the one the session's layer assignments and
          * baseline were computed for. Never rebuilt, never normalized. */
         image: proxy.path,
         produced: produced
      });

      say("  batch " + batches + "  iter " + result.iteration +
                      "  applied " + produced.length + "/" + instructions.length +
                      "  D = " + round(result.distance, 4) +
                      "  (" + batchTime.text + ")" +
                      (result.converged ? "   [converged]" : ""));

      instructions = result.instructions;
      if (result.converged || instructions.length === 0)
         break;
   }

   /* --- 5. Report everything, including the parts that went wrong. --------- */
   say("");
   say("stopped because: " + result.convergence_reason);
   say("iterations     : " + result.iterations +
                   "   batches: " + batches +
                   "   candidates applied: " + candidatesApplied);
   say("time           : PixInsight " + round(piTime, 1) + "s, " +
                   "total " + totalTime.text + " over " + sidecarCalls +
                   " sidecar calls");

   reportExecutorFailures();
   var counts = reportGuardrailLog(result.guardrail_log || []);

   /* --- 6. The recipe — the §12 audit artifact. ---------------------------- */
   writeText(RECIPE_FILE, JSON.stringify({
      improved: result.improved,
      source_path: image,
      proxy_path: proxy.path,
      reference_id: result.reference_id,
      baseline_distance: result.baseline_distance,
      distance: result.distance,
      convergence_reason: result.convergence_reason,
      iterations: result.iterations,
      recipe: result.recipe,
      pixinsight_steps: result.pixinsight_steps,
      guardrail_log: result.guardrail_log,
      executor_failures: executorFailures
   }, null, 2));

   say("");
   if (!result.improved) {
      /* The correct outcome on an already-good image, and it must read that way.
       * The distance is a VALLEY — zero at the reference, rising in BOTH
       * directions — so an image that starts near the floor has nowhere to go
       * but up, and the loop declining is the fail-safe working, not failing. */
      note("  DECLINED — nothing beat the input by the required margin.");
      say("  baseline D = " + round(result.baseline_distance, 4) +
                      "; best candidate did not improve on it.");
      say("  YOUR FILE WAS NOT MODIFIED: " + image);
      say("  (no processes were applied to it; the candidates under " +
                      WORK + " are search intermediates and can be deleted.)");
      say("  recipe written to " + RECIPE_FILE + " (empty, by design)");
      return { ok: true, improved: false, distance: result.baseline_distance,
               baseline: result.baseline_distance, iterations: result.iterations,
               discarded: counts.discarded, noted: counts.noted,
               reference: result.reference_id };
   }

   note("  IMPROVED  D " + round(result.baseline_distance, 4) +
                  " -> " + round(result.distance, 4));
   say("  recipe (" + result.pixinsight_steps.length +
                   " stock processes) written to " + RECIPE_FILE);
   for (var k = 0; k < result.pixinsight_steps.length; ++k) {
      var step = result.pixinsight_steps[k];
      say("    " + (k + 1) + ". " + step.process + "   " + step.action);
   }
   say("  best candidate: " + result.result_path);
   say("  YOUR FILE WAS NOT MODIFIED: " + image);
   say("  The recipe is the deliverable — applying it to the " +
                   "full-resolution master is a separate, deliberate step.");
   return { ok: true, improved: true, distance: result.distance,
            baseline: result.baseline_distance, iterations: result.iterations,
            discarded: counts.discarded, noted: counts.noted,
            reference: result.reference_id };
}

try {
   var out = main();
   writeText(STATUS_FILE,
             "PASS improved=" + out.improved +
             " distance=" + out.distance +
             " baseline=" + out.baseline +
             " iterations=" + out.iterations +
             " reference=" + out.reference +
             " discarded=" + out.discarded +
             " noted=" + out.noted +
             " executor_failures=" + executorFailures.length +
             "\n");
   note("<b>AutoContrast Optimize complete</b>");
   flushReport("AutoContrast — Optimize: PASS improved=" + out.improved);
} catch (e) {
   /* Headless PixInsight writes nothing useful to stdout, so these two files are
    * the only channels that survive the run. Both get the failure — and the
    * report gets everything that had been reported BEFORE it, which is where
    * the reason usually is. */
   writeText(STATUS_FILE, "FAIL " + e.message +
             " executor_failures=" + executorFailures.length + "\n");
   /* No executor-failure summary here: `noteExecutorFailure` already recorded
    * each one as it happened, so the report carries them without a second pass
    * that could double-print them when main() threw after its own summary. */
   _record("");
   _record("FAILED: " + e.message);
   flushReport("AutoContrast — Optimize: FAILED");
   console.criticalln("AutoContrast Optimize FAILED: " + e.message);
}
