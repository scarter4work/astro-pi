// Gaia Depth Grade — interactive Script Dialog.
//
// Two-phase: Prepare (solve + StarXTerminator split + detect + Gaia + match, ~50s,
// once per target, cached) -> Preview (fast re-render per gain change) -> Execute
// (full-res render + screen-blend recombine into a new window).
//
// Run from PixInsight: Script > Execute Script... -> this file. Prepare works on a
// DUPLICATE of the target so your master is never modified.
#feature-id    scarter4work > Gaia Depth Grade
#script-id     GaiaDepthGrade
#feature-info  Physically-grounded depth grade for astrophotographs using Gaia distances.
#include "gaia_depth_grade_lib.jsh"
#include <pjsr/Sizer.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/FrameStyle.jsh>

#define UI_TITLE       "Gaia Depth Grade"
#define PREVIEW_W      760
#define PREVIEW_H      420
#define INSET_PX       256        // 1:1 inset size in full-res pixels
#define GDG_MAG_LIMIT  18.0       // G-mag cut for the offline cone query (matches the sidecar default)

function duplicateWindow(srcWindow, newId) {
   var img = srcWindow.mainView.image;
   var w = new ImageWindow(img.width, img.height, img.numberOfChannels,
                           img.bitsPerSample, img.isReal, img.isColor, newId);
   w.mainView.beginProcess(UndoFlag_NoSwapFile);
   w.mainView.image.assign(img);
   w.mainView.endProcess();
   w.keywords = srcWindow.keywords;   // carry RA/DEC/FOCALLEN so the solver can seed
   return w;
}

function GaiaDepthGradeDialog() {
   this.__base__ = Dialog;
   this.__base__();

   var dialog = this;
   this.minWidth = 1040;

   // --- state ---
   this.targetWindow = null;
   this.prepared = false;
   this.fullW = 0; this.fullH = 0;
   this.insetCx = 0; this.insetCy = 0;
   this.previewBmp = null;
   this.insetBmp = null;
   this.cacheDir = TMP_DIR + "/ui";
   this.starsPath = this.cacheDir + "/stars_full.fits";
   this.starlessPath = this.cacheDir + "/starless_full.fits";
   this.fullPng = this.cacheDir + "/preview_full.png";
   this.insetPng = this.cacheDir + "/preview_inset.png";

   // live render params. Defaults tuned for "more depth, low grain" (validated on a
   // real HaO3 master): lead with SIZE (additive near-star glow — clean depth cue,
   // no noise) and keep CONTRAST low (unsharp is the grain driver). Measured ~3x the
   // foreground-star prominence of the old 0.5/0.4/0.3/0.3 at slightly LOWER grain.
   this.gBrightness = 0.65; this.gSize = 0.85; this.gContrast = 0.12; this.gSaturation = 0.4;
   this.baseSigma = 2.0; this.pLow = 5.0; this.pHigh = 95.0;
   // Nebula (starless-layer) depth — opt-in. Structure drives where it varies;
   // the Gaia depth budget scales how strong it is, so it matches the star depths.
   this.nebEnabled = false; this.nebStrength = 1.0; this.nebAtmos = 1.0; this.nebStructure = 1.0;
   // prepare-time params
   this.detectSigma = 6.0; this.matchTol = 4.0;

   // ---------- target row ----------
   this.viewList = new ViewList(this);
   this.viewList.getMainViews();
   this.viewList.onViewSelected = function (view) {
      if (view && !view.isNull) dialog.setTargetWindow(view.window);
   };
   this.browseButton = new PushButton(this);
   this.browseButton.text = "Browse…";
   this.browseButton.onClick = function () { dialog.onBrowse(); };

   this.targetSizer = new HorizontalSizer;
   this.targetSizer.spacing = 6;
   var targetLabel = new Label(this);
   targetLabel.text = "Target:"; targetLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.targetSizer.add(targetLabel);
   this.targetSizer.add(this.viewList, 100);
   this.targetSizer.add(this.browseButton);

   // ---------- preview controls ----------
   this.previewControl = new Control(this);
   this.previewControl.setFixedSize(PREVIEW_W, PREVIEW_H);
   this.previewControl.toolTip = "Recombined preview (autostretched). Click to move the 1:1 inset.";
   this.previewControl.onPaint = function () {
      var g = new Graphics(this);
      g.fillRect(0, 0, this.width, this.height, new Brush(0xff101010));
      if (dialog.previewBmp != null) g.drawBitmap(0, 0, dialog.previewBmp);
      if (dialog.previewStale) {
         g.pen = new Pen(0xa0ff8000, 2);
         g.drawRect(1, 1, this.width - 1, this.height - 1);
      }
      g.end();
   };
   this.previewControl.onMousePress = function (x, y, button, buttons, modifiers) {
      if (!dialog.prepared || dialog.previewBmp == null) return;
      var scale = dialog.fullW / dialog.previewBmp.width;   // full-res px per preview px
      dialog.insetCx = Math.round(x * scale);
      dialog.insetCy = Math.round(y * scale);
      dialog.doPreview();
   };

   this.insetControl = new Control(this);
   this.insetControl.setFixedSize(INSET_PX, INSET_PX);
   this.insetControl.toolTip = "1:1 zoom of the clicked region.";
   this.insetControl.onPaint = function () {
      var g = new Graphics(this);
      g.fillRect(0, 0, this.width, this.height, new Brush(0xff101010));
      if (dialog.insetBmp != null) g.drawBitmap(0, 0, dialog.insetBmp);
      g.end();
   };

   this.previewSizer = new HorizontalSizer;
   this.previewSizer.spacing = 8;
   this.previewSizer.add(this.previewControl);
   var insetBox = new VerticalSizer;
   var insetLabel = new Label(this); insetLabel.text = "1:1 inset";
   insetBox.add(insetLabel);
   insetBox.add(this.insetControl);
   insetBox.addStretch();
   this.previewSizer.add(insetBox);
   this.previewSizer.addStretch();

   // ---------- live sliders ----------
   function liveSlider(label, value, lo, hi, prec, setter) {
      var nc = new NumericControl(dialog);
      nc.label.text = label;
      nc.label.minWidth = 110;
      nc.setRange(lo, hi);
      nc.setPrecision(prec);
      nc.setValue(value);
      nc.onValueUpdated = function (v) { setter(v); dialog.markPreviewStale(); };
      return nc;
   }
   this.ncBrightness = liveSlider("Brightness:", this.gBrightness, 0, 2, 2, function (v) { dialog.gBrightness = v; });
   this.ncSize       = liveSlider("Size:",       this.gSize,       0, 2, 2, function (v) { dialog.gSize = v; });
   this.ncContrast   = liveSlider("Contrast:",   this.gContrast,   0, 2, 2, function (v) { dialog.gContrast = v; });
   this.ncSaturation = liveSlider("Saturation:", this.gSaturation, 0, 2, 2, function (v) { dialog.gSaturation = v; });
   this.ncBaseSigma  = liveSlider("Halo sigma:", this.baseSigma,   0.5, 6, 2, function (v) { dialog.baseSigma = v; });
   this.ncPLow       = liveSlider("p_low:",      this.pLow,        0, 50, 1, function (v) { dialog.pLow = v; });
   this.ncPHigh      = liveSlider("p_high:",     this.pHigh,       50, 100, 1, function (v) { dialog.pHigh = v; });

   // Nebula depth (opt-in): grade the starless layer too.
   this.nebCheck = new CheckBox(this);
   this.nebCheck.text = "Grade the nebula too (depth from structure, scaled to Gaia)";
   this.nebCheck.checked = this.nebEnabled;
   this.nebCheck.onCheck = function (c) { dialog.nebEnabled = c; dialog.markPreviewStale(); };
   this.ncNebStrength  = liveSlider("Nebula depth:", this.nebStrength,  0, 2, 2, function (v) { dialog.nebStrength = v; });
   this.ncNebAtmos     = liveSlider("  atmospheric:", this.nebAtmos,    0, 2, 2, function (v) { dialog.nebAtmos = v; });
   this.ncNebStructure = liveSlider("  structure:",   this.nebStructure, 0, 2, 2, function (v) { dialog.nebStructure = v; });

   this.liveGroup = new VerticalSizer;
   this.liveGroup.spacing = 4;
   var liveTitle = new Label(this); liveTitle.text = "Look (live — Preview to apply)"; liveTitle.styleSheet = "font-weight: bold;";
   this.liveGroup.add(liveTitle);
   this.liveGroup.add(this.ncBrightness); this.liveGroup.add(this.ncSize);
   this.liveGroup.add(this.ncContrast); this.liveGroup.add(this.ncSaturation);
   this.liveGroup.add(this.ncBaseSigma); this.liveGroup.add(this.ncPLow); this.liveGroup.add(this.ncPHigh);
   this.liveGroup.add(this.nebCheck);
   this.liveGroup.add(this.ncNebStrength); this.liveGroup.add(this.ncNebAtmos); this.liveGroup.add(this.ncNebStructure);

   // ---------- prepare-time fields ----------
   function prepEdit(label, value, lo, hi, prec, setter) {
      var ne = new NumericEdit(dialog);
      ne.label.text = label;
      ne.label.minWidth = 150;
      ne.setRange(lo, hi);
      ne.setPrecision(prec);
      ne.setValue(value);
      ne.onValueUpdated = function (v) { setter(v); dialog.markPrepareStale(); };
      return ne;
   }
   this.neDetect = prepEdit("detect_threshold_sigma:", this.detectSigma, 1, 30, 1, function (v) { dialog.detectSigma = v; });
   this.neMatch  = prepEdit("match_tolerance_px:",     this.matchTol,    1, 12, 1, function (v) { dialog.matchTol = v; });

   this.prepGroup = new VerticalSizer;
   this.prepGroup.spacing = 4;
   var prepTitle = new Label(this); prepTitle.text = "Matching (changing these needs re-Prepare)"; prepTitle.styleSheet = "font-weight: bold;";
   this.prepGroup.add(prepTitle);
   this.prepGroup.add(this.neDetect); this.prepGroup.add(this.neMatch);

   // Offline (local Gaia DR3) vs online (Bailer-Jones). Default offline: it uses
   // the local .xpsd database PI already needs for plate-solving, so it's instant
   // and needs no network — at the cost of parallax-based (not Bailer-Jones)
   // distances and the coverage of whichever DR3 database is installed.
   this.offlineCheck = new CheckBox(this);
   this.offlineCheck.text = "Offline (local Gaia DR3) — fast";
   this.offlineCheck.checked = true;
   this.offlineCheck.toolTip = "Checked: cone-query the local Gaia DR3 database (instant, offline; " +
      "parallax distances). Unchecked: the online Bailer-Jones catalogue (more sources, but the " +
      "ESA archive can be slow or down).";
   this.prepGroup.add(this.offlineCheck);
   this.prepGroup.addStretch();

   this.controlsSizer = new HorizontalSizer;
   this.controlsSizer.spacing = 18;
   this.controlsSizer.add(this.liveGroup, 100);
   this.controlsSizer.add(this.prepGroup, 100);

   // ---------- buttons + status ----------
   this.prepareButton = new PushButton(this);
   this.prepareButton.text = "Prepare";
   this.prepareButton.toolTip = "Solve + StarXTerminator + Gaia match. Run once per target " +
      "(dense fields: the first Gaia query can take a few minutes, then it's cached).";
   this.prepareButton.onClick = function () { dialog.doPrepare(); };

   this.previewButton = new PushButton(this);
   this.previewButton.text = "Preview / Update";
   this.previewButton.enabled = false;
   this.previewButton.onClick = function () { dialog.doPreview(); };

   this.executeButton = new PushButton(this);
   this.executeButton.text = "Execute";
   this.executeButton.enabled = false;
   this.executeButton.onClick = function () { dialog.doExecute(); };

   this.closeButton = new PushButton(this);
   this.closeButton.text = "Close";
   this.closeButton.onClick = function () { dialog.cancel(); };

   this.buttonSizer = new HorizontalSizer;
   this.buttonSizer.spacing = 8;
   this.buttonSizer.add(this.prepareButton);
   this.buttonSizer.add(this.previewButton);
   this.buttonSizer.addStretch();
   this.buttonSizer.add(this.executeButton);
   this.buttonSizer.add(this.closeButton);

   this.statusLabel = new Label(this);
   this.statusLabel.text = "Pick a target and click Prepare.";
   this.statusLabel.frameStyle = FrameStyle_Box;
   this.statusLabel.textAlignment = TextAlign_Left | TextAlign_VertCenter;
   this.statusLabel.minHeight = 28;

   this.sizer = new VerticalSizer;
   this.sizer.margin = 8;
   this.sizer.spacing = 8;
   this.sizer.add(this.targetSizer);
   this.sizer.add(this.previewSizer);
   this.sizer.add(this.controlsSizer);
   this.sizer.add(this.buttonSizer);
   this.sizer.add(this.statusLabel);

   this.windowTitle = UI_TITLE;
   this.adjustToContents();

   // default target = active window, if any
   var aw = ImageWindow.activeWindow;
   if (aw && !aw.isNull) this.setTargetWindow(aw);
}

GaiaDepthGradeDialog.prototype = new Dialog;

GaiaDepthGradeDialog.prototype.setStatus = function (msg) {
   this.statusLabel.text = msg;
   console.noteln("[GDG] " + msg);
   processEvents();
};

GaiaDepthGradeDialog.prototype.setError = function (e) {
   var msg = (e && e.message) ? e.message : String(e);
   this.statusLabel.text = "ERROR: " + msg;
   console.criticalln("[GDG] ERROR: " + msg);
   processEvents();
};

GaiaDepthGradeDialog.prototype.setTargetWindow = function (window) {
   this.targetWindow = window;
   if (this.viewList) this.viewList.currentView = window.mainView;
   this.markPrepareStale();
   this.setStatus("Target: " + window.mainView.id + " — click Prepare.");
};

GaiaDepthGradeDialog.prototype.markPrepareStale = function () {
   this.prepared = false;
   if (this.previewButton) this.previewButton.enabled = false;
   if (this.executeButton) this.executeButton.enabled = false;
   this.previewStale = true;
   if (this.previewControl) this.previewControl.repaint();
};

GaiaDepthGradeDialog.prototype.markPreviewStale = function () {
   this.previewStale = true;
   if (this.previewControl) this.previewControl.repaint();
};

GaiaDepthGradeDialog.prototype.onBrowse = function () {
   var ofd = new OpenFileDialog;
   ofd.caption = "Open master";
   ofd.loadImageFilters();
   if (ofd.execute() && ofd.fileNames.length > 0) {
      var arr = ImageWindow.open(ofd.fileNames[0]);
      if (arr && arr.length > 0) {
         arr[0].show();
         if (this.viewList) this.viewList.getMainViews();
         this.setTargetWindow(arr[0]);
      } else {
         this.setError("failed to open " + ofd.fileNames[0]);
      }
   }
};

GaiaDepthGradeDialog.prototype.gainsArg = function () {
   return format("%.4f,%.4f,%.4f,%.4f", this.gBrightness, this.gSize, this.gContrast, this.gSaturation);
};

GaiaDepthGradeDialog.prototype.renderArgs = function () {
   var a = ["--gains", this.gainsArg(),
            "--p-low", format("%.3f", this.pLow),
            "--p-high", format("%.3f", this.pHigh),
            "--base-sigma", format("%.3f", this.baseSigma)];
   if (this.nebEnabled && this.nebStrength > 0)
      a = a.concat(["--nebula-strength", format("%.4f", this.nebStrength),
                    "--nebula-atmos", format("%.4f", this.nebAtmos),
                    "--nebula-structure", format("%.4f", this.nebStructure)]);
   return a;
};

GaiaDepthGradeDialog.prototype.doPrepare = function () {
   if (this.targetWindow == null || this.targetWindow.isNull) {
      this.setError("no target selected");
      return false;
   }
   var dup = null;
   try {
      this.cursor = new Cursor(StdCursor_Wait);
      gdgEnsureDir(this.cacheDir);
      this.setStatus("Preparing: solving + StarXTerminator + Gaia match (~50s)…");

      dup = duplicateWindow(this.targetWindow, this.targetWindow.mainView.id + "_gdgwork");
      var split = solveAndSplit(dup);   // mutates dup -> starless; returns stars window

      if (!split.stars.saveAs(this.starsPath, false, false, false, false))
         throw new Error("failed to save " + this.starsPath);
      if (!split.starless.saveAs(this.starlessPath, false, false, false, false))
         throw new Error("failed to save " + this.starlessPath);

      var img = this.targetWindow.mainView.image;
      this.fullW = img.width; this.fullH = img.height;
      this.insetCx = Math.round(this.fullW / 2); this.insetCy = Math.round(this.fullH / 2);

      var prepareArgs = ["prepare", gdgQuote(this.starsPath), gdgQuote(this.cacheDir),
                         "--config", gdgQuote(this.writePrepareConfig())];
      if (this.offlineCheck.checked) {
         // Offline: ask the sidecar for the field footprint (reusing its tested
         // WCS math), cone-query the local Gaia DR3, and feed the result in.
         this.setStatus("Querying local Gaia DR3 (offline)…");
         var fpLine = runCapture(sidecarCmd(["footprint", gdgQuote(this.starsPath)]));
         var fp = parseFootprint(fpLine);   // robust to PI merging stderr warnings into stdout
         var tsv = this.cacheDir + "/gaia_local.tsv";
         var n = queryLocalGaiaDR3(fp[0], fp[1], fp[2], GDG_MAG_LIMIT, tsv);
         this.setStatus("Local Gaia DR3: " + n + " sources. Running prepare (detect + match)…");
         prepareArgs.push("--gaia-tsv", gdgQuote(tsv));
      } else {
         this.setStatus("Running prepare (detect + online Gaia query). On a dense field the " +
                        "async query can take several minutes the first time; the result is " +
                        "cached so re-runs are fast…");
      }
      run(sidecarCmd(prepareArgs));

      // Defense-in-depth: a prepare that fails must NOT look like success. If the
      // Gaia query or detection threw, the sidecar exited non-zero (run() throws)
      // — but verify the artifacts exist before we trust them and run preview, so
      // a missing prep.npz/qa.json surfaces loudly instead of failing in preview.
      if (!File.exists(this.cacheDir + "/prep.npz") ||
          !File.exists(this.cacheDir + "/qa.json"))
         throw new Error("prepare produced no output (prep.npz/qa.json missing) — the " +
                         "Gaia query or detection step failed. See the console above " +
                         "for the sidecar error.");

      split.stars.forceClose();
      split.starless.forceClose();   // == dup; reopened from FITS at Execute time
      dup = null;

      var qa = this.readQa();
      this.prepared = true;
      this.previewButton.enabled = true;
      this.executeButton.enabled = true;
      this.setStatus(this.qaSummary(qa));
      this.doPreview();
      return true;
   } catch (e) {
      if (dup != null) dup.forceClose();
      this.setError(e);
      return false;
   } finally {
      this.cursor = new Cursor(StdCursor_Arrow);
   }
};

GaiaDepthGradeDialog.prototype.writePrepareConfig = function () {
   // emit a tiny TOML so prepare uses the dialog's detect/match values
   var path = this.cacheDir + "/prepare.toml";
   var toml = format("detect_threshold_sigma = %.3f\nmatch_tolerance_px = %.3f\n",
                     this.detectSigma, this.matchTol);
   File.writeTextFile(path, toml);
   return path;
};

GaiaDepthGradeDialog.prototype.readQa = function () {
   var qaPath = this.cacheDir + "/qa.json";
   if (!File.exists(qaPath)) return null;
   var f = new File;
   f.openForReading(qaPath);
   var buf = f.read(DataType_ByteArray, f.size);
   f.close();
   return JSON.parse(buf.utf8ToString(0, buf.length));
};

GaiaDepthGradeDialog.prototype.qaSummary = function (qa) {
   if (qa == null) return "Prepared, but QA summary is unavailable (qa.json missing/unreadable).";
   var s = format("Prepared: %d/%d matched (rate %.2f, offset %.1f px)",
                  qa.n_matched, qa.n_detected, qa.match_rate, qa.median_offset_px);
   if (qa.low_match_warning) s += "  ⚠ LOW MATCH — depth grade unreliable";
   return s;
};

GaiaDepthGradeDialog.prototype.insetRegion = function () {
   var x = Math.max(0, Math.min(this.fullW - INSET_PX, this.insetCx - INSET_PX / 2));
   var y = Math.max(0, Math.min(this.fullH - INSET_PX, this.insetCy - INSET_PX / 2));
   return format("%d,%d,%d,%d", x, y, INSET_PX, INSET_PX);
};

GaiaDepthGradeDialog.prototype.doPreview = function () {
   if (!this.prepared) { this.setError("Prepare first"); return false; }
   try {
      this.cursor = new Cursor(StdCursor_Wait);
      this.setStatus("Rendering preview…");
      var args = ["preview", gdgQuote(this.cacheDir), gdgQuote(this.starsPath),
                  gdgQuote(this.starlessPath), gdgQuote(this.fullPng),
                  "--inset", gdgQuote(this.insetPng), "--region", this.insetRegion(),
                  "--max-width", String(PREVIEW_W)].concat(this.renderArgs());
      run(sidecarCmd(args));
      this.previewBmp = new Bitmap(this.fullPng);
      this.insetBmp = new Bitmap(this.insetPng);
      this.previewStale = false;
      this.previewControl.repaint();
      this.insetControl.repaint();
      this.setStatus("Preview updated. Adjust the look, Preview again, or Execute.");
      return true;
   } catch (e) {
      this.setError(e);
      return false;
   } finally {
      this.cursor = new Cursor(StdCursor_Arrow);
   }
};

GaiaDepthGradeDialog.prototype.doExecute = function () {
   if (!this.prepared) { this.setError("Prepare first"); return false; }
   var starlessWin = null, gradedWin = null;
   try {
      this.cursor = new Cursor(StdCursor_Wait);
      this.setStatus("Executing: full-res render + recombine…");
      var gradedPath = this.cacheDir + "/graded_full.fits";
      run(sidecarCmd(["render", gdgQuote(this.cacheDir), gdgQuote(this.starsPath),
                 gdgQuote(gradedPath)].concat(this.renderArgs())));

      // Nebula depth (opt-in): grade the starless layer to a FITS and blend THAT
      // instead of the raw starless, so the recombine carries the nebula depth too.
      var starlessForBlend = this.starlessPath;
      if (this.nebEnabled && this.nebStrength > 0) {
         var nebPath = this.cacheDir + "/nebula_full.fits";
         run(sidecarCmd(["nebula", gdgQuote(this.cacheDir), gdgQuote(this.starlessPath),
                    gdgQuote(nebPath)].concat(this.renderArgs())));
         starlessForBlend = nebPath;
      }

      starlessWin = ImageWindow.open(starlessForBlend)[0];
      gradedWin = ImageWindow.open(gradedPath)[0];

      var PM = new PixelMath;
      PM.expression = "~((~" + starlessWin.mainView.id + ") * (~" + gradedWin.mainView.id + "))";
      PM.createNewImage = true;
      PM.newImageId = this.targetWindow.mainView.id + "_depthgraded";
      PM.executeOn(starlessWin.mainView);

      starlessWin.forceClose();
      gradedWin.forceClose();

      var resultWin = ImageWindow.windowById(PM.newImageId);
      resultWin.show();
      this.setStatus("Done: created " + PM.newImageId);
      return true;
   } catch (e) {
      if (starlessWin != null) starlessWin.forceClose();
      if (gradedWin != null) gradedWin.forceClose();
      this.setError(e);
      return false;
   } finally {
      this.cursor = new Cursor(StdCursor_Arrow);
   }
};

function main() {
   var dialog = new GaiaDepthGradeDialog();
   dialog.execute();
}

// Only auto-run the dialog when executed directly (a driver/validation script can
// #include this file and drive the methods without popping the modal dialog).
#ifndef GDG_NO_MAIN
main();
#endif
