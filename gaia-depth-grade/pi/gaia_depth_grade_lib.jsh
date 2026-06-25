// gaia_depth_grade_lib.jsh — shared PJSR helpers for the Gaia Depth Grade harness
// and interactive dialog: the ImageSolver library preamble, an external-process
// runner, WCS-keyword export, StarXTerminator split, and a solve+split helper.
#ifndef __GAIA_DEPTH_GRADE_LIB_JSH
#define __GAIA_DEPTH_GRADE_LIB_JSH
#script-id     GaiaDepthGradeLib

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

#define TMP_DIR  "/tmp/gaia_depth_grade"

// --- Frozen sidecar (the Python core, shipped as a PyInstaller binary) ---------
// The public user has no Python; the Python core is fetched on first run from a
// GitHub Release asset and cached. These three are bumped per sidecar release;
// SIDECAR_SHA256 MUST match the uploaded asset byte-for-byte (verified below).
#define SIDECAR_VERSION  "1.0.0"
#define SIDECAR_TGZ      "gaia-depth-grade-sidecar-1.0.0-linux-x64.tar.gz"
#define SIDECAR_URL      "https://github.com/scarter4work/astro-pi/releases/download/gaia-depth-grade-v1.0.0/gaia-depth-grade-sidecar-1.0.0-linux-x64.tar.gz"
#define SIDECAR_SHA256   "dc34af141f091687a50b9a972f9422fcc114a2c86467316d4d1c7d3d764ca934"
#define SIDECAR_NAME     "gaia-depth-grade-sidecar"

function gdgEnsureDir(d) {
   if (!File.directoryExists(d))   // createDirectory throws if it exists
      File.createDirectory(d, true);
}

function gdgQuote(s) { return "\"" + s + "\""; }

// PI exports its own LD_LIBRARY_PATH (/opt/PixInsight/bin/lib) to every child
// process, so an external binary loads PI's bundled libs (e.g. an older libssl
// without OPENSSL_3.2.0) instead of the system's and fails to link. Run every
// external command through `env -u LD_LIBRARY_PATH` so curl, the frozen sidecar,
// and the coreutils below all link against the system libraries.
function scrub(cmd) { return "/usr/bin/env -u LD_LIBRARY_PATH " + cmd; }

// ExternalProcess(commandLineString) starts a shell command and runs it; the
// constructor wants a single string (an array throws "String expected"), and
// p.start(program,args) returns a non-bool here, so use the canonical form:
// construct with a quoted command line, waitForFinished(), check exitCode.
function run(cmd) {
   var p = new ExternalProcess(scrub(cmd));
   p.waitForFinished();
   if (p.exitCode != 0)
      throw new Error("command failed (exit " + p.exitCode + "): " + cmd +
                      "\n" + p.stderr);
}

// Like run(), but capture and return trimmed stdout (used to probe --version and
// to read sha256sum's digest). Throws loudly on a non-zero exit.
function runCapture(cmd) {
   var p = new ExternalProcess(scrub(cmd));
   p.waitForFinished();
   if (p.exitCode != 0)
      throw new Error("command failed (exit " + p.exitCode + "): " + cmd + "\n" + p.stderr);
   return p.stdout.toString().trim();
}

// Download `url` to `path`. GitHub Release assets 302-redirect to a signed CDN
// URL, and PI's NetworkTransfer does NOT follow redirects (no CURLOPT_FOLLOWLOCATION
// in NetworkTransfer.cpp) — it "succeeds" with the empty redirect body. So we shell
// out to curl, then wget; both follow redirects. Phase 1 is linux-x64 only, where
// one of the two is effectively always present; we fail loudly if neither works.
function tryDownload(cmd) {
   try {
      var p = new ExternalProcess(scrub(cmd));
      p.waitForFinished();
      return p.exitCode == 0;
   } catch (e) {
      return false;   // program not found / failed to start -> try the next tool
   }
}

function downloadTo(url, path) {
   if (tryDownload("curl -fsSL -o " + gdgQuote(path) + " " + gdgQuote(url)))
      return;
   if (tryDownload("wget -q -O " + gdgQuote(path) + " " + gdgQuote(url)))
      return;
   throw new Error("sidecar download failed for " + url +
                   " — install curl or wget and ensure network access, then retry.");
}

// Resolve the frozen sidecar binary, fetching+verifying+caching it on first run.
// Cache: ~/.astro-pi/gaia-depth-grade/bin/<SIDECAR_NAME>. Loud on every failure
// path (unsupported platform, download error, sha256 mismatch, bad archive) —
// never returns a partial/placeholder binary.
function sidecarBin() {
   if (CoreApplication.platform != "Linux")
      throw new Error("the Gaia Depth Grade sidecar is not yet available for platform '" +
                      CoreApplication.platform + "' (Phase 1 ships linux-x64 only).");

   var binDir = File.homeDirectory + "/.astro-pi/gaia-depth-grade/bin";
   var bin = binDir + "/" + SIDECAR_NAME;

   // Cached and version-matched? Use it. A version mismatch (or a binary that
   // won't report its version) is treated as missing -> re-download.
   if (File.exists(bin)) {
      var cached = "";
      try { cached = runCapture(gdgQuote(bin) + " --version"); } catch (e) { cached = ""; }
      if (cached == SIDECAR_VERSION)
         return bin;
   }

   gdgEnsureDir(binDir);
   console.noteln("Gaia Depth Grade: fetching sidecar v" + SIDECAR_VERSION + " (~95 MB, first run only)...");
   var tmpTgz = File.systemTempDirectory + "/" + SIDECAR_TGZ;
   downloadTo(SIDECAR_URL, tmpTgz);

   var got = runCapture("sha256sum " + gdgQuote(tmpTgz)).split(/\s+/)[0];
   if (got != SIDECAR_SHA256) {
      File.remove(tmpTgz);
      throw new Error("sidecar sha256 mismatch (download corrupt or tampered): got " +
                      got + ", expected " + SIDECAR_SHA256 + " — aborting.");
   }

   run("tar -xzf " + gdgQuote(tmpTgz) + " -C " + gdgQuote(binDir));
   File.remove(tmpTgz);
   if (!File.exists(bin))
      throw new Error("sidecar archive did not contain " + SIDECAR_NAME);
   run("chmod +x " + gdgQuote(bin));

   var installed = runCapture(gdgQuote(bin) + " --version");
   if (installed != SIDECAR_VERSION)
      throw new Error("installed sidecar reports version '" + installed +
                      "', expected '" + SIDECAR_VERSION + "'");
   console.noteln("Gaia Depth Grade: sidecar ready at " + bin);
   return bin;
}

// Build a sidecar command line: <sidecar> <subcommand> <args...>. The frozen
// binary takes the subcommand directly (no `-m`), and resolving it triggers the
// first-run fetch above. Args are pre-quoted by callers.
function sidecarCmd(args) {
   var s = gdgQuote(sidecarBin());
   for (var i = 0; i < args.length; ++i)
      s += " " + args[i];
   return s;
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
   if (typeof StarXTerminator == "undefined")
      throw new Error("StarXTerminator is required (a paid PixInsight module) but is not " +
                      "installed. Install it, then re-run Gaia Depth Grade.");
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

// Plate-solve `window`, split with StarXTerminator, stamp the WCS onto the stars
// layer. Returns { stars: <stars ImageWindow>, starless: <window, now starless> }.
function solveAndSplit(window) {
   var view = window.mainView;
   var solver = new ImageSolver;
   solver.Init(window, false);
   if (!solver.SolveImage(window))
      throw new Error("plate solve failed for " + view.id);
   var starsWin = splitStars(view);   // SXT leaves starless in `window`
   writeWcsKeywords(starsWin, solver.metadata);
   return { stars: starsWin, starless: window };
}

#endif // __GAIA_DEPTH_GRADE_LIB_JSH
