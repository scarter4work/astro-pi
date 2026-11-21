#include <pjsr/UndoFlag.jsh>
/*
 * Headless colour-calibration step for linear mosaic chains (mosaic -> SPFC/SPCC -> BXT -> NXT).
 * Shared copy: chains under ~/astro_work used to copy this script from project to project, and every copy
 * neutralised the background against the WHOLE frame. On a frame-filling nebula that "background" is mostly
 * nebula, so the real sky comes out colour-cast: NGC 2244 2026-11-20 sky R/G 0.79 (whole frame) vs 1.02
 * (nebula-free corner); NGC 2244 2026-11-17 sky R/G 0.96 (R 2.8e-4 below G, 8-12x the sky's pixel noise).
 * The background reference is now a nebula-free region by default.
 *
 * Run through pi-headless.sh, e.g.
 *   PixInsight -n=<slot> --automation-mode --no-startup-scripts --force-exit \
 *     -r="tools/astro-chain/spcc.js,in=<linear.xisf>,out=<_2_spcc.xisf>,log=<log>,mode=nb,filter=hao3"
 *
 * Args (key=value, comma-separated):
 *   in, out, log   required
 *   mode=nb|bb     narrowband (dual/quad-band through OSC) or broadband SPCC
 *   filter=hao3|lqef  narrowband passband preset; or bands=Rwl/Rbw,Gwl/Gbw,Bwl/Bbw (nm). One is required for nb.
 *                  hao3 = Askar ColorMagic D1: Ha 656.3/8.5, OIII 500.7/6.5 (R = Ha, G = B = OIII)
 *                  lqef = Optolong L-QEF quad-band: R = Ha+SII 664/24, G = B = Hb+OIII 493/20 (widths are the
 *                         spans of the line groups; Optolong publishes no FWHM)
 *   spfc=0|1       run SpectrophotometricFluxCalibration first (same passbands)
 *   wcs=<file>     copy keywords + astrometric solution from this file first
 *   bgroi=auto     (default) background reference = darkest tile of a bgtiles x bgtiles grid
 *                  (tile score = mean over channels of the tile's non-zero median / image median; tiles with
 *                  more than 5 % zero/no-data pixels are skipped). Check the logged candidates against a
 *                  view of the image.
 *   bgroi=x0:y0:x1:y1  explicit nebula-free box
 *   bgroi=whole    whole frame (only for fields with no extended emission; logged as such)
 *   bgtiles=8      grid size for bgroi=auto
 * After SPCC the log lists R/G and B/G of the reference and of the next-darkest tiles, and prints
 * "WARNING sky colour" when any of them is more than 0.05 from 1.
 */
var A = {}; for (var i = 0; i < jsArguments.length; ++i) { var kv = jsArguments[i].split("="); A[kv[0]] = kv.slice(1).join("="); }
var f = new File; f.createForWriting(A.log);
function log(s){ f.outTextLn(s); f.flush(); }

var PRESETS = { hao3: [[656.3, 8.5], [500.7, 6.5], [500.7, 6.5]],
                lqef: [[664.0, 24.0], [493.0, 20.0], [493.0, 20.0]] };

function nbBands() {
   if (A.bands) {
      var b = A.bands.split(",").map(function (p) { return p.split("/").map(Number); });
      if (b.length != 3 || b.some(function (p) { return p.length != 2 || isNaN(p[0]) || isNaN(p[1]); }))
         throw new Error("bands= must be Rwl/Rbw,Gwl/Gbw,Bwl/Bbw");
      return b;
   }
   if (A.filter && PRESETS[A.filter.toLowerCase()]) return PRESETS[A.filter.toLowerCase()];
   throw new Error("mode=nb needs filter=" + Object.keys(PRESETS).join("|") + " or bands=");
}

var MAX_NODATA = 0.05;    // tiles with more zero (no-data) pixels than this are not candidates

// k-th smallest of a[0..n) (Hoare quickselect, in place).
function select(a, n, k) {
   var lo = 0, hi = n - 1;
   while (lo < hi) {
      var p = a[(lo + hi) >> 1], i = lo, j = hi;
      while (i <= j) {
         while (a[i] < p) ++i;
         while (a[j] > p) --j;
         if (i <= j) { var t = a[i]; a[i] = a[j]; a[j] = t; ++i; --j; }
      }
      if (k <= j) hi = j; else if (k >= i) lo = i; else break;
   }
   return a[k];
}

// Per-channel median of the non-zero pixels in t, plus the zero fraction of channel 0. Zero = no-data margin
// (mosaic notches, uncropped edges); SPCC's own background estimate rejects such pixels as far below
// backgroundLow sigma, so a reference box may hold a few of them.
function tileStats(img, t) {
   var r = new Rect(t.x0, t.y0, t.x1, t.y1), buf = new Float32Array(r.width * r.height), meds = [], zf = 0;
   for (var c = 0; c < img.numberOfChannels; ++c) {
      img.getSamples(buf, r, c);
      var nz = 0;
      for (var k = 0; k < buf.length; ++k) if (buf[k] > 0) buf[nz++] = buf[k];
      if (c == 0) zf = 1 - nz / buf.length;
      if (nz == 0) return { zf: 1, meds: null };
      meds.push(select(buf, nz, nz >> 1));        // PJSR's engine has no TypedArray.prototype.sort
   }
   return { zf: zf, meds: meds };
}

// Tiles of a bgtiles x bgtiles grid with at most MAX_NODATA zero pixels, darkest first.
function darkTiles(img, n) {
   var W = img.width, H = img.height, C = img.numberOfChannels;
   var tw = Math.floor(W / n), th = Math.floor(H / n), tiles = [], g = [];
   img.resetSelections();
   for (var c = 0; c < C; ++c) { img.selectedChannel = c; g.push(img.median()); }   // scale only
   img.resetSelections();
   for (var j = 0; j < n; ++j)
      for (var i = 0; i < n; ++i) {
         var t = { x0: i * tw, y0: j * th, x1: (i + 1) * tw, y1: (j + 1) * th }, st = tileStats(img, t);
         if (st.zf > MAX_NODATA) continue;
         var s = 0;
         for (var c = 0; c < C; ++c) s += st.meds[c] / g[c];
         t.score = s / C; t.meds = st.meds; t.zf = st.zf;
         tiles.push(t);
      }
   tiles.sort(function (a, b) { return a.score - b.score; });
   return tiles;
}

function tileMedians(img, t) { return tileStats(img, t).meds; }

function roiText(t) { return t.x0 + ":" + t.y0 + ":" + t.x1 + ":" + t.y1; }

Console.beginLog();
try {
   if (!A["in"] || !A.out || !A.log) throw new Error("in=, out= and log= are required");
   if (A.mode != "nb" && A.mode != "bb") throw new Error("mode= must be nb or bb");
   var w = ImageWindow.open(A["in"])[0], v = w.mainView;
   if (A.wcs) { var s = ImageWindow.open(A.wcs)[0]; w.keywords = s.keywords; w.copyAstrometricSolution(s); s.forceClose(); }
   log("input " + A["in"] + " " + v.image.width + "x" + v.image.height + " wcs=" + w.hasAstrometricSolution);
   if (!w.hasAstrometricSolution) throw new Error("no astrometric solution");
   var bands = (A.mode == "nb") ? nbBands() : null;

   // Background reference, chosen on the input before anything changes it.
   var bg = A.bgroi || "auto", ref = null, checks = [];
   if (bg == "auto") {
      var n = A.bgtiles ? parseInt(A.bgtiles) : 8;
      var tiles = darkTiles(v.image, n);
      if (tiles.length == 0) throw new Error("bgroi=auto: no data-only tile in a " + n + "x" + n + " grid; pass bgroi=x0:y0:x1:y1");
      ref = tiles[0]; checks = tiles.slice(1, 6);
      log("background reference: auto, " + n + "x" + n + " grid, " + tiles.length + " tiles with <= " + (100 * MAX_NODATA) +
          " % no-data; darkest first (score, box, no-data %, input medians):");
      for (var k = 0; k < Math.min(6, tiles.length); ++k)
         log(format("  %.4f %s  %.1f%%  %s", tiles[k].score, roiText(tiles[k]), 100 * tiles[k].zf,
             tiles[k].meds.map(function (x) { return x.toFixed(6); }).join(" ")));
   } else if (bg != "whole") {
      var r = bg.split(":").map(Number);
      if (r.length != 4 || r.some(isNaN)) throw new Error("bgroi= must be auto, whole or x0:y0:x1:y1");
      ref = { x0: r[0], y0: r[1], x1: r[2], y1: r[3] };
      checks = darkTiles(v.image, A.bgtiles ? parseInt(A.bgtiles) : 8).slice(0, 5);
      log("background reference: explicit ROI " + roiText(ref));
   } else {
      checks = darkTiles(v.image, A.bgtiles ? parseInt(A.bgtiles) : 8).slice(0, 5);
      log("background reference: WHOLE FRAME (bgroi=whole) -- wrong for any frame with extended emission");
   }

   var S = new SpectrophotometricColorCalibration;
   var IDEAL = S.deviceQECurve;
   if (A.spfc == "1") {
      var F = new SpectrophotometricFluxCalibration;
      F.narrowbandMode = (A.mode == "nb");
      if (F.narrowbandMode) {
         F.redFilterWavelength = bands[0][0];   F.redFilterBandwidth = bands[0][1];
         F.greenFilterWavelength = bands[1][0]; F.greenFilterBandwidth = bands[1][1];
         F.blueFilterWavelength = bands[2][0];  F.blueFilterBandwidth = bands[2][1];
      } else {
         F.redFilterTrCurve = S.redFilterTrCurve;     F.redFilterName = S.redFilterName;
         F.greenFilterTrCurve = S.greenFilterTrCurve; F.greenFilterName = S.greenFilterName;
         F.blueFilterTrCurve = S.blueFilterTrCurve;   F.blueFilterName = S.blueFilterName;
      }
      F.deviceQECurve = IDEAL; F.deviceQECurveName = S.deviceQECurveName;
      F.generateGraphs = false;
      var fok = F.executeOn(v);
      log("SPFC executeOn=" + fok + " narrowband=" + F.narrowbandMode);
      if (!fok) throw new Error("SPFC failed");
   }
   S.narrowbandMode = (A.mode == "nb");
   if (S.narrowbandMode) {
      S.redFilterWavelength = bands[0][0];   S.redFilterBandwidth = bands[0][1];
      S.greenFilterWavelength = bands[1][0]; S.greenFilterBandwidth = bands[1][1];
      S.blueFilterWavelength = bands[2][0];  S.blueFilterBandwidth = bands[2][1];
      log(format("NB bands R %.1f/%.1f G %.1f/%.1f B %.1f/%.1f nm", bands[0][0], bands[0][1], bands[1][0], bands[1][1], bands[2][0], bands[2][1]));
   }
   S.generateGraphs = false; S.generateStarMaps = false; S.generateTextFiles = false;
   S.neutralizeBackground = true;
   if (ref) {
      S.backgroundUseROI = true;
      S.backgroundROIX0 = ref.x0; S.backgroundROIY0 = ref.y0; S.backgroundROIX1 = ref.x1; S.backgroundROIY1 = ref.y1;
   } else S.backgroundUseROI = false;
   log("SPCC mode=" + (S.narrowbandMode ? "narrowband" : "broadband (" + S.redFilterName + "/" + S.greenFilterName + "/" + S.blueFilterName + ", " + S.deviceQECurveName + ")") +
       " whiteRef=" + S.whiteReferenceName + " catalog=" + S.catalogId + " backgroundUseROI=" + S.backgroundUseROI +
       (ref ? " ROI " + roiText(ref) : ""));
   v.beginProcess(UndoFlag_NoSwapFile);
   var ok = S.executeOn(v);
   v.endProcess();
   log("SPCC executeOn=" + ok);
   if (!ok) throw new Error("SPCC failed");

   var img = v.image, bad = 0;
   for (var c = 0; c < img.numberOfChannels; ++c) { img.selectedChannel = c; log(format("  ch%d median=%.6f MAD=%.3e", c, img.median(), img.MAD())); }
   img.resetSelections();
   log("sky colour after SPCC (tile medians):");
   var all = (ref ? [ref] : []).concat(checks);
   for (var k = 0; k < all.length; ++k) {
      var m = tileMedians(img, all[k]), rg = m[0] / m[1], bgr = m[2] / m[1];
      if (Math.abs(rg - 1) > 0.05 || Math.abs(bgr - 1) > 0.05) ++bad;
      log(format("  %s %s  R/G %.3f  B/G %.3f", (k == 0 && ref) ? "ref  " : "check", roiText(all[k]), rg, bgr));
   }
   if (bad) log("WARNING sky colour: " + bad + " of " + all.length + " sky tiles more than 0.05 from neutral; inspect and pass bgroi=x0:y0:x1:y1");
   w.saveAs(A.out, false, false, false, false);
   log("SAVED " + A.out);
   w.forceClose();
} catch (e) { log("ERROR: " + e + " line=" + e.lineNumber); }
var L = Console.endLog();
log("---- console ----"); log(L.toString());
f.close();
