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
//
// ImageSolver writes its catalogue star list to STAR_CSV_FILE during every solve
// (and removes it afterwards). Its own default is a predictable name in the
// shared temporary directory, where a symbolic link planted by another user
// would redirect the write; ours lives in the private working directory.
#define SETTINGS_MODULE "SOLVER"
#define STAR_CSV_FILE   (gdgWorkDir() + format( "/stars-%03d.csv", CoreApplication.instance ))
#include <pjsr/DataType.jsh>
#include <pjsr/FileAttribute.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/UndoFlag.jsh>
#include <pjsr/TextAlign.jsh>
#include "/opt/PixInsight/src/scripts/AdP/WCSmetadata.jsh"
#include "/opt/PixInsight/src/scripts/AdP/AstronomicalCatalogs.jsh"
#define USE_SOLVER_LIBRARY true
#include "/opt/PixInsight/src/scripts/AdP/ImageSolver.js"

// --- The private working directory -------------------------------------------
// Every file this product writes outside the PixInsight installation lives in
// one per-user directory, $XDG_CACHE_HOME/gaia-depth-grade (by default
// ~/.cache/gaia-depth-grade), mode 0700:
//    ui/            the dialog's layers, prep cache and previews
//    gaia_cache/    online Gaia query results
//    headless/      gaia_depth_grade.js's files
//    stars-NNN.csv  ImageSolver's star list, while it solves
// It is never a shared, predictable location such as /tmp/<name>: files there
// can be pre-created, altered or replaced by symbolic links by any local user,
// and the prep cache read back from it is trusted. Every directory is checked
// WITHOUT following links (FileInfo reports a symbolic link as one): it must be
// a real directory owned by this user, and is restricted to 0700. Anything else
// stops the script; nothing is ever written through it. Only this user can
// create entries inside a 0700 directory, so no other user can plant a link or
// a file next to ours.
var __gdgWorkDir = null;

// A TOML basic string holding `s` (paths go into the sidecar's config files).
function gdgTomlString(s) {
   return "\"" + s.replace(/\\/g, "\\\\").replace(/"/g, "\\\"") + "\"";
}

function gdgUserId() {
   var uid = new FileInfo(File.homeDirectory).userId;
   if (!(uid >= 0))
      throw new Error("cannot determine the owner of the home directory " + File.homeDirectory);
   return uid;
}

#define GDG_PRIVATE_MODE (FilePermission_Read | FilePermission_Write | FilePermission_Execute)

// `dir` must be (or is created as) a real directory owned by `uid`, mode 0700.
function gdgPrivateDir(dir, uid) {
   var fi = new FileInfo(dir);   // lstat semantics: a symbolic link is never followed
   if (!fi.exists) {
      File.createDirectory(dir, false);
      fi.refresh(dir);
   }
   if (fi.isSymbolicLink)
      throw new Error("Gaia Depth Grade: refusing to use " + dir + ": it is a symbolic link (to " +
                      fi.symbolicLinkTarget + "), not a directory. Remove it and run the script again.");
   if (!fi.isDirectory)
      throw new Error("Gaia Depth Grade: refusing to use " + dir + ": it is not a directory.");
   if (fi.userId != uid)
      throw new Error("Gaia Depth Grade: refusing to use " + dir + ": it is owned by user id " +
                      fi.userId + ", not by you (" + uid + ").");
   if ((fi.attributes & FilePermission_Mask) != GDG_PRIVATE_MODE) {
      File.setPermissions(dir, GDG_PRIVATE_MODE);
      fi.refresh(dir);
      if ((fi.attributes & FilePermission_Mask) != GDG_PRIVATE_MODE)
         throw new Error("Gaia Depth Grade: cannot restrict " + dir + " to mode 0700.");
   }
   return dir;
}

// The private working directory, or its subdirectory `sub` ("ui", "gaia_cache",
// "headless"), verified as above on first use in this script run.
function gdgWorkDir(sub) {
   if (__gdgWorkDir == null) {
      var uid = gdgUserId();
      var base = getEnvironmentVariable("XDG_CACHE_HOME");
      if (!base || base.charAt(0) != "/")
         base = File.homeDirectory + "/.cache";
      var bi = new FileInfo(base);
      if (bi.exists && bi.isSymbolicLink && bi.userId != uid)
         throw new Error("Gaia Depth Grade: refusing to use " + base + ": it is a symbolic link owned by " +
                         "user id " + bi.userId + ", not by you.");
      if (!bi.exists)
         File.createDirectory(base, true);
      __gdgWorkDir = gdgPrivateDir(base + "/gaia-depth-grade", uid);
   }
   if (!sub)
      return __gdgWorkDir;
   return gdgPrivateDir(__gdgWorkDir + "/" + sub, gdgUserId());
}

// --- The sidecar: the Python core, frozen into one executable -----------------
// PixInsight's update system installs it from the signed astro-pi repository
// (package "Gaia Depth Grade sidecar", linux-x64) as
// <PixInsight>/bin/gaia-depth-grade-sidecar. These scripts never download,
// unpack, chmod or install code: they run that installed file, and stop with
// an explanation when it is missing, cannot run, or is not the version they
// were released with. SIDECAR_VERSION is bumped together with the sidecar
// package (release.sh refuses a mismatch).
#define SIDECAR_VERSION  "1.0.8"
#define SIDECAR_NAME     "gaia-depth-grade-sidecar"
var GDG_UPDATE_HINT = "Install or update \"Gaia Depth Grade\" and \"Gaia Depth Grade sidecar\" " +
                      "from the astro-pi repository with Resources > Updates > Check for Updates, " +
                      "then restart PixInsight.";

// Do NOT use waitForFinished(): in PI it can return BEFORE a long-running child
// actually exits, so exitCode reads as 0 prematurely — a `prepare` whose Gaia
// query takes minutes (or fails) would look like success and we'd march on to
// preview with no prep.npz. Instead spin on the process state while pumping the
// event loop, exactly as PI's own bundled scripts (SetiAstro, BlindSolver2000)
// do for long external tools. This blocks until the child truly finishes and
// keeps the UI responsive.
function spinUntilDone(p) {
   while (p.isStarting) processEvents();
   while (p.isRunning) processEvents();
}

// Start `program args...` and wait for it. The working directory is the private
// working directory, so nothing a child writes by relative path can land in
// PixInsight's own directory (PI's working directory is its bin/) or anywhere
// shared. Returns trimmed output; throws
// loudly on a failed start or a non-zero exit.
//
// PI exports its own LD_LIBRARY_PATH (<PixInsight>/bin/lib:<PixInsight>/bin) to
// every child process, so a child would load PI's bundled libraries (e.g. an
// older libssl) instead of the system's and fail to link. /usr/bin/env -u
// LD_LIBRARY_PATH starts the child without it.
//
// IMPORTANT: PI's ExternalProcess MERGES the child's stderr into `.stdout`
// (verified: `.stderr` comes back empty), so the returned text can contain
// stderr diagnostics ahead of the real payload, and error reports must quote
// `.stdout` or the real error (e.g. a Python traceback) is lost. Callers that
// parse structured output must tolerate leading noise -- see parseFootprint().
function gdgExec(program, args) {
   var argv = ["-u", "LD_LIBRARY_PATH", program].concat(args);
   var p = new ExternalProcess;
   p.workingDirectory = gdgWorkDir();
   p.start("/usr/bin/env", argv);
   spinUntilDone(p);
   var out = p.stdout.toString().trim();
   if (p.exitCode != 0)
      throw new Error("command failed (exit " + p.exitCode + "): /usr/bin/env " +
                      argv.join(" ") + "\n" + out);
   return out;
}

// Extract [ra, dec, radiusDeg] from `footprint` output. Because PI merges the
// sidecar's stderr into stdout, the blob is "<astropy FITS warnings…>\n<ra dec r>".
// The numeric line is printed AFTER the FITS is opened, so it is reliably last;
// scan from the end for the first line that parses as three finite numbers. Fails
// loudly rather than letting a non-numeric token become NaN (the cause of the
// "0 sources" bug: a NaN cone center silently returns an empty catalogue).
function parseFootprint(blob) {
   var lines = blob.split(/\r?\n/);
   for (var i = lines.length - 1; i >= 0; --i) {
      var t = lines[i].trim();
      if (!t) continue;
      var parts = t.split(/\s+/);
      if (parts.length >= 3) {
         var ra = parseFloat(parts[0]), dec = parseFloat(parts[1]), r = parseFloat(parts[2]);
         if (isFinite(ra) && isFinite(dec) && isFinite(r))
            return [ra, dec, r];
      }
   }
   throw new Error("could not parse a center RA/Dec/radius from footprint output:\n" + blob);
}

// The installed sidecar: <PixInsight>/bin/gaia-depth-grade-sidecar, deployed by
// the update package. Verified once per script run (an extra --version call per
// subcommand would unpack the whole frozen program a second time). Every failure
// names the path and says how to fix it; nothing is ever fetched or repaired here.
var __gdgSidecarVerified = null;

function sidecarBin() {
   if (__gdgSidecarVerified != null)
      return __gdgSidecarVerified;
   if (CoreApplication.platform != "Linux")
      throw new Error("Gaia Depth Grade's sidecar program is only available for Linux x64; " +
                      "this is " + CoreApplication.platform + ".");
   var bin = CoreApplication.binDirPath + "/" + SIDECAR_NAME;
   if (!File.exists(bin))
      throw new Error("Gaia Depth Grade's sidecar program is not installed (expected " + bin +
                      "). " + GDG_UPDATE_HINT);
   gdgWorkDir();   // the sidecar runs in it: a refused directory must say so, not "cannot run"
   var reported;
   try {
      reported = gdgExec(bin, ["--version"]);
   } catch (e) {
      throw new Error("Gaia Depth Grade's sidecar program at " + bin + " cannot run: " +
                      e.message + "\n" + GDG_UPDATE_HINT);
   }
   // The version is printed alone on the last line; anything before it is noise
   // a child may emit on stderr (merged into stdout by PI).
   var lines = reported.split(/\r?\n/);
   var version = lines[lines.length - 1].trim();
   if (version != SIDECAR_VERSION)
      throw new Error("Gaia Depth Grade's sidecar program at " + bin + " is version '" + version +
                      "', but these scripts need version " + SIDECAR_VERSION + ". " + GDG_UPDATE_HINT);
   __gdgSidecarVerified = bin;
   return bin;
}

// Run the installed sidecar: `args` is its argv after the program name
// (<subcommand> <args...>), plain strings -- no shell, so no quoting.
function runSidecar(args) {
   return gdgExec(sidecarBin(), args);
}

// --- Offline local Gaia DR3 -------------------------------------------------
// Query the local Gaia DR3 database (the same .xpsd files PI uses for
// plate-solving, so every user of this tool already has them) via PI's Gaia
// process, and write a clean ra/dec/parallax/G TSV for the sidecar. This is the
// fast, offline alternative to the online Bailer-Jones query — distances come
// from parallax (see local_gaia.py). Source record layout (Gaia.sources rows):
// [ra, dec, parallax, pmRA, pmDec, magG, magBP, magRP, flags, flux...].

// Pull the configured DR3/DR3SP .xpsd paths out of PI's core settings file.
// A `new Gaia` inherits the user's configured databaseFilePaths in an
// interactive session; this is the fallback when that list is empty.
function _extractGaiaDbPaths(txt, key) {
   var out = [];
   var re = new RegExp(key + '\\d+"\\s+t="s">([^<]+\\.xpsd)', "g");
   var m;
   while ((m = re.exec(txt)) != null)
      out.push(m[1]);
   return out;
}

function discoverGaiaDbPaths() {
   var dir = File.homeDirectory + "/.PixInsight";
   if (!File.directoryExists(dir))
      return [];
   var settings = [];
   var ff = new FileFind;
   if (ff.begin(dir + "/core-*-pxi.settings")) {
      do { if (!ff.isDirectory) settings.push(dir + "/" + ff.name); } while (ff.next());
   }
   for (var i = 0; i < settings.length; ++i) {
      var txt = File.readTextFile(settings[i]);
      // Prefer base DR3 (full astrometric catalogue) over the DR3SP subset.
      var dr3 = _extractGaiaDbPaths(txt, "DR3DatabaseFilePath");
      if (dr3.length) return dr3;
      var sp = _extractGaiaDbPaths(txt, "DR3SPDatabaseFilePath");
      if (sp.length) return sp;
   }
   return [];
}

// Does this set of .xpsd paths hold the DR3SP (spectrophotometry) subset?
// PI's official subset databases are named `gdr3sp-*.xpsd`; a user can drop them
// into the base-"DR3" slot, so the filename — not PI's slot label — is the only
// trustworthy signal of which data release the files actually contain.
function _looksLikeSP(paths) {
   for (var i = 0; i < paths.length; ++i)
      if (/gdr3sp|dr3sp|[_\-.]sp[_\-.]/i.test(paths[i]))
         return true;
   return false;
}

// Configure + cone-search, trying the file-appropriate data release first and the
// other as a fallback. Critical: configuring DR3SP files under base DataRelease_3
// reports isValid=true yet every search returns zero rows — so we must NOT trust
// isValid as success. For a field we know is covered (it plate-solved), "0 sources"
// means we picked the wrong release, so we retry the other one before giving up.
function _searchGaia(P, paths, ra, dec, radiusDeg, magHigh) {
   var sp = Gaia.prototype.DataRelease_3_SP, base = Gaia.prototype.DataRelease_3;
   var order = _looksLikeSP(paths) ? [sp, base] : [base, sp];
   var why = "configure never succeeded";
   for (var i = 0; i < order.length; ++i) {
      P.dataRelease = order[i];
      P.command = "configure";
      P.executeGlobal();
      if (!P.isValid) {
         why = "configure invalid for data release " + order[i];
         console.noteln("  Gaia release " + order[i] + ": configure invalid"); continue;
      }
      P.command = "search";
      P.centerRA = ra; P.centerDec = dec; P.radius = radiusDeg;
      P.magnitudeLow = -5.0; P.magnitudeHigh = magHigh;
      P.sourceLimit = 4294967295;
      P.requiredFlags = 0; P.inclusionFlags = 0; P.exclusionFlags = 0;
      P.verbosity = 0; P.generateTextOutput = false;
      if (!P.executeGlobal()) {
         why = "search failed for data release " + order[i];
         console.noteln("  Gaia release " + order[i] + ": search executeGlobal()=false"); continue;
      }
      console.noteln("  Gaia release " + order[i] + " @ (" + format("%.4f", ra) + "," +
                     format("%.4f", dec) + ") r=" + format("%.3f", radiusDeg) +
                     "deg mag<=" + magHigh + ": " + P.sources.length + " sources");
      if (P.sources.length > 0) return P.sources;   // real data — this release matches the files
      why = "data release " + order[i] + " configured but returned 0 sources";
   }
   throw new Error("local Gaia cone search returned no sources for any data release (" + why +
                   "). The configured .xpsd files may not cover this field, or PI's DR3 / DR3SP " +
                   "database slots are pointed at the wrong files.");
}

function queryLocalGaiaDR3(ra, dec, radiusDeg, magHigh, outPath) {
   if (typeof Gaia == "undefined")
      throw new Error("PixInsight's Gaia process is unavailable — cannot use offline mode. " +
                      "Uncheck 'Offline' to use the online catalogue.");
   var P = new Gaia;
   // Trust DISCOVERY over inheritance. A fresh automation `new Gaia` inherits an
   // EMPTY databaseFilePaths and discovery yields the working .xpsd set (verified:
   // 19,385 sources). But in an interactive GUI session `new Gaia` inherits the
   // user's persistent process state, whose databaseFilePaths has been observed to
   // configure-valid yet return 0 sources — the actual cause of the "0 sources"
   // failure. Discovery parses the same database PI plate-solves with, so it is
   // deterministic; use it first and fall back to the inherited list only if
   // discovery finds nothing.
   var inheritedLen = P.databaseFilePaths.length;
   var paths = discoverGaiaDbPaths();
   var pathSrc = "discovered";
   if (paths.length == 0) {
      paths = [];
      for (var p = 0; p < inheritedLen; ++p) paths.push(P.databaseFilePaths[p][0]);
      pathSrc = "inherited";
   }
   if (paths.length == 0)
      throw new Error("No local Gaia DR3 database is configured in PixInsight (the one " +
                      "plate-solving uses). Configure it, or uncheck 'Offline' to use the " +
                      "online catalogue.");
   var rows = [];
   for (var i = 0; i < paths.length; ++i) rows.push([paths[i]]);
   P.databaseFilePaths = rows;   // override any inherited list with the resolved one
   console.noteln("offline Gaia DR3: using " + paths.length + " " + pathSrc +
                  " db file(s) (interactive instance inherited " + inheritedLen + " path(s))");
   var s = _searchGaia(P, paths, ra, dec, radiusDeg, magHigh);
   var f = new File;
   f.createForWriting(outPath);
   var buf = "ra\tdec\tparallax\tphot_g_mean_mag\n";   // sidecar parses these 4 columns
   for (var j = 0; j < s.length; ++j) {
      var r = s[j];
      buf += r[0] + "\t" + r[1] + "\t" + r[2] + "\t" + r[5] + "\n";
      if (buf.length > 1048576) { f.outText(buf); buf = ""; }   // flush ~1 MB chunks
   }
   f.outText(buf); f.close();
   console.noteln("offline Gaia DR3: " + s.length + " sources -> " + outPath);
   return s.length;
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

   // Y-AXIS CONVENTION FLIP (load-bearing). PI's solver expresses CRPIX2/CD in
   // its own TOP-origin raster, but ImageWindow.saveAs() writes the pixel data
   // BOTTOM-up (FITS standard). If we emitted the solver values verbatim, every
   // star would land mirrored across the image midline and astropy would match
   // detections to sky at chance level (measured match_rate ~0.10, ~random for
   // the field density). Emit the WCS already in FITS bottom-up convention:
   // reflect CRPIX2 about the image (H+1-CRPIX2) and negate the Y (column-2) CD
   // terms (CD1_2, CD2_2). Verified on a real Cygnus master: match_rate
   // 0.10 -> 0.73 with a 0.24 px median residual. CRVAL/PV/LONPOLE/LATPOLE are
   // projection parameters, unaffected by a pixel-axis reflection.
   var H = win.mainView.image.height;

   add("RADESYS", "'" + metadata.referenceSystem + "'", "Reference system of celestial coordinates");
   add("CTYPE1", wcs.ctype1, "Axis1 projection");
   add("CTYPE2", wcs.ctype2, "Axis2 projection");
   add("CRPIX1", format("%.8f", wcs.crpix1), "Axis1 reference pixel");
   add("CRPIX2", format("%.8f", (H + 1) - wcs.crpix2), "Axis2 reference pixel (FITS bottom-up)");
   if (wcs.crval1 != null) add("CRVAL1", format("%.16f", wcs.crval1), "Axis1 reference value");
   if (wcs.crval2 != null) add("CRVAL2", format("%.16f", wcs.crval2), "Axis2 reference value");
   if (wcs.pv1_1 != null) add("PV1_1", format("%.16f", wcs.pv1_1), "Native longitude of the reference point");
   if (wcs.pv1_2 != null) add("PV1_2", format("%.16f", wcs.pv1_2), "Native latitude of the reference point");
   if (wcs.lonpole != null) add("LONPOLE", format("%.16f", wcs.lonpole), "Longitude of the celestial pole");
   if (wcs.latpole != null) add("LATPOLE", format("%.16f", wcs.latpole), "Latitude of the celestial pole");
   add("CD1_1", format("%.16f", wcs.cd1_1), "Scale matrix (1,1)");
   add("CD1_2", format("%.16f", -wcs.cd1_2), "Scale matrix (1,2) (Y flipped to FITS bottom-up)");
   add("CD2_1", format("%.16f", wcs.cd2_1), "Scale matrix (2,1)");
   add("CD2_2", format("%.16f", -wcs.cd2_2), "Scale matrix (2,2) (Y flipped to FITS bottom-up)");
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
