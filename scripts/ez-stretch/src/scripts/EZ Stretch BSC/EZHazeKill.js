// ============================================================================
// EZHazeKill.js - Flat-background haze detector + corrector
// ============================================================================
//
// Removes the flat, slightly-colored background haze (a per-channel sky
// pedestal and color cast) that remains AFTER background extraction
// (GraXpert / DBE / GradientCorrection). It measures the sky on the darkest
// tiles of the image, then subtracts a constant per channel through PixelMath
// (undoable, expressions visible in the history):
//
//   sky_c    median, over the darkest 10% of 64 px tiles (ranked by tile
//            luminance), of each tile's channel-c median
//   sigma_c  1.4826 x the median tile MAD of the same tiles
//   cast_c   sky_c - mean(sky)                       (removed x Neutralize)
//   T_eff    max(Target, 4 x max sigma)              (noise floor)
//   ped      max(0, mean(sky) - T_eff)               (removed x Amount; never
//                                                     raised)
//   u        x - (Neutralize x cast_c + Amount x ped)
//   out      u                     where u >= eps
//            eps x exp(u/eps - 1)  where u <  eps    (soft toe, eps = 2 sigma)
//
// The toe is monotonic and continuous with its slope at eps, so nothing is
// clipped to zero. It does NOT model a gradient - run GraXpert/DBE first.
// Works on linear and on stretched images.
//
// Copyright (c) 2026 EZ Stretch BSC
// SPDX-License-Identifier: CC-BY-NC-4.0
// Licensed under CC BY-NC 4.0  -  http://creativecommons.org/licenses/by-nc/4.0/
// NonCommercial use only. This file is NOT covered by the BSD 3-Clause
// license of the rest of the astro-pi repository.
//
// ============================================================================

// The "Id : menu path" form names the script EZHazeKill (so PixInsight finds
// its documentation at doc/scripts/EZHazeKill/EZHazeKill.html) and replaces
// #script-id: code signing allows only one identification directive.
#feature-id    EZHazeKill : scarter4work > EZ Haze Kill
#feature-info  Removes flat background haze (residual sky pedestal \
               and color cast) left after gradient extraction.

#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/TextAlign.jsh>

#define TITLE   "EZ Haze Kill"
#define VERSION "1.1.0"

// Fixed model constants (see the documentation).
#define HK_TILE            64      // tile size, pixels
#define HK_SKY_PERCENTILE  10      // darkest tiles used for the sky, percent
#define HK_FLOOR_SIGMA     4.0     // the sky is never pushed below this many sigma
#define HK_TOE_SIGMA       2.0     // soft toe starts this many sigma above zero
#define HK_STRETCHED_NOISE 0.03    // sky noise above this fraction of the sky level reads as a
                                   // stretched image (measured: linear masters 0.4-1.1%,
                                   // stretched images 6-38%; the sky level alone overlaps)

var jsAutoGC = true;

// ============================================================================
// Parameters
// ============================================================================
function HazeKillParameters() {
   this.targetView  = undefined;
   this.amount      = 0.5;    // fraction of the sky pedestal removed
   this.neutralize  = 1.0;    // fraction of the sky color cast removed
   this.targetLevel = 0.05;   // lowest level the sky is brought down to

   this.save = function() {
      Parameters.set("amount", this.amount);
      Parameters.set("neutralize", this.neutralize);
      Parameters.set("targetLevel", this.targetLevel);
   };
   this.load = function() {
      // Icons saved by 1.0.x carry only targetLevel; the other two keep
      // their defaults.
      if (Parameters.has("amount"))
         this.amount = Parameters.getReal("amount");
      if (Parameters.has("neutralize"))
         this.neutralize = Parameters.getReal("neutralize");
      if (Parameters.has("targetLevel"))
         this.targetLevel = Parameters.getReal("targetLevel");
   };
}
var parameters = new HazeKillParameters();

// ============================================================================
// Sky estimate
// ============================================================================

// k-th smallest of a[0..n-1] by in-place selection (Wirth). On return every
// element before index k is <= a[k] and every element after it is >= a[k].
// (PJSR's engine has no typed-array sort, and a full sort per tile is slow.)
function hkSelect(a, n, k) {
   var l = 0, m = n - 1;
   while (l < m) {
      var x = a[k], i = l, j = m;
      do {
         while (a[i] < x) ++i;
         while (x < a[j]) --j;
         if (i <= j) {
            var t = a[i]; a[i] = a[j]; a[j] = t;
            ++i; --j;
         }
      } while (i <= j);
      if (j < k) l = i;
      if (k < i) m = j;
   }
   return a[k];
}

// Median of a[0..n-1], reordering a: the mean of the two middle values for an
// even n (numpy's definition).
function hkMedianInPlace(a, n) {
   var h = n >> 1;
   var v = hkSelect(a, n, h);
   if (n & 1)
      return v;
   var lo = a[0];
   for (var i = 1; i < h; ++i)
      if (a[i] > lo)
         lo = a[i];
   return 0.5*(lo + v);
}

function hkMedian(values) {
   var a = new Float64Array(values.length);
   for (var i = 0; i < a.length; ++i)
      a[i] = values[i];
   return hkMedianInPlace(a, a.length);
}

// Percentile with linear interpolation between ranks (numpy's default
// "linear" method).
function hkPercentile(values, q) {
   var a = [];
   for (var i = 0; i < values.length; ++i)
      a.push(values[i]);
   a.sort(function(x, y) { return x - y; });
   var pos = q/100*(a.length - 1);
   var i0 = Math.floor(pos);
   var f = pos - i0;
   return (i0 + 1 < a.length) ? a[i0] + f*(a[i0 + 1] - a[i0]) : a[i0];
}

// Per-tile, per-channel median and MAD over HK_TILE x HK_TILE tiles. Partial
// tiles at the right and bottom edges are left out; an image smaller than one
// tile is measured as a single tile.
function hkTileStats(img) {
   var nc = img.numberOfChannels;
   var tw = HK_TILE, th = HK_TILE;
   if (img.width < tw || img.height < th) {
      tw = img.width;
      th = img.height;
   }
   var nx = Math.floor(img.width/tw);
   var ny = Math.floor(img.height/th);
   var n = tw*th;
   var buf = new Float64Array(n);
   var med = [], mad = [];
   for (var c = 0; c < nc; ++c) {
      med.push(new Float64Array(nx*ny));
      mad.push(new Float64Array(nx*ny));
   }
   for (var ty = 0, t = 0; ty < ny; ++ty)
      for (var tx = 0; tx < nx; ++tx, ++t) {
         var r = new Rect(tx*tw, ty*th, tx*tw + tw, ty*th + th);
         for (var c = 0; c < nc; ++c) {
            img.getSamples(buf, r, c);
            var m = hkMedianInPlace(buf, n);
            for (var i = 0; i < n; ++i)
               buf[i] = Math.abs(buf[i] - m);
            med[c][t] = m;
            mad[c][t] = 1.4826*hkMedianInPlace(buf, n);
         }
      }
   return { nc: nc, count: nx*ny, med: med, mad: mad };
}

// Sky level and noise per channel from the darkest HK_SKY_PERCENTILE % of the
// tiles, ranked by tile luminance (the mean of the channel medians).
function hkMeasureSky(img) {
   var s = hkTileStats(img);
   var L = new Float64Array(s.count);
   for (var t = 0; t < s.count; ++t) {
      var sum = 0;
      for (var c = 0; c < s.nc; ++c)
         sum += s.med[c][t];
      L[t] = sum/s.nc;
   }
   var cut = hkPercentile(L, HK_SKY_PERCENTILE);
   var sel = [];
   for (var t = 0; t < s.count; ++t)
      if (L[t] <= cut)
         sel.push(t);
   var sky = [], sigma = [];
   for (var c = 0; c < s.nc; ++c) {
      var a = [], b = [];
      for (var k = 0; k < sel.length; ++k) {
         a.push(s.med[c][sel[k]]);
         b.push(s.mad[c][sel[k]]);
      }
      sky.push(hkMedian(a));
      sigma.push(hkMedian(b));
   }
   return { sky: sky, sigma: sigma, tiles: s.count, skyTiles: sel.length };
}

// ============================================================================
// Model: what Apply subtracts
// ============================================================================
function hkModel(m, amount, neutralize, target) {
   var nc = m.sky.length;
   var skyL = 0, sigMax = 0;
   for (var c = 0; c < nc; ++c) {
      skyL += m.sky[c];
      sigMax = Math.max(sigMax, m.sigma[c]);
   }
   skyL /= nc;
   var floorLevel = HK_FLOOR_SIGMA*sigMax;
   var tEff = Math.max(target, floorLevel);
   var ped = Math.max(0, skyL - tEff);            // never raise the pedestal
   var cast = [], offset = [];
   for (var c = 0; c < nc; ++c) {
      cast.push(m.sky[c] - skyL);
      offset.push(neutralize*cast[c] + amount*ped);
   }
   return {
      sky: m.sky, sigma: m.sigma, skyL: skyL, sigmaMax: sigMax,
      floorLevel: floorLevel, target: target, tEff: tEff, ped: ped,
      cast: cast, offset: offset,
      eps: Math.max(1.0e-6, HK_TOE_SIGMA*sigMax),
      amount: amount, neutralize: neutralize,
      stretched: sigMax > HK_STRETCHED_NOISE*skyL,
      tiles: m.tiles, skyTiles: m.skyTiles
   };
}

function hkLabels(nc) {
   return (nc == 3) ? ["R", "G", "B"] : ["K"];
}

function hkReport(r) {
   var lab = hkLabels(r.sky.length);
   console.writeln(format("Sky from the darkest %d%% of %d tiles (%d tiles):",
                          HK_SKY_PERCENTILE, r.tiles, r.skyTiles));
   for (var c = 0; c < r.sky.length; ++c)
      console.writeln(format("   %s: sky %.5f   sigma %.5f   cast %+.5f   subtract %+.5f",
                             lab[c], r.sky[c], r.sigma[c], r.cast[c], r.offset[c]));
   console.writeln(format("   sky level %.5f, target %.5f, %.0f-sigma noise floor %.5f",
                          r.skyL, r.target, HK_FLOOR_SIGMA, r.floorLevel));
   if (r.floorLevel > r.target)
      console.writeln(format("   The noise floor (%.5f) is above the target: the sky stays at or above it.",
                             r.floorLevel));
   if (r.ped <= 0)
      console.writeln("   The sky is already at or below the target: the pedestal is left as it is (never raised).");
   else
      console.writeln(format("   Pedestal above the target: %.5f, removing %.0f%% of it (Amount %.2f).",
                             r.ped, 100*r.amount, r.amount));
   console.writeln(format("   Color cast removal: %.0f%% (Neutralize %.2f).", 100*r.neutralize, r.neutralize));
   if (r.stretched)
      console.noteln(format("Note: the sky noise is %.1f%% of the sky level (more than %.0f%%), so this looks like " +
                            "a stretched (non-linear) image. That is supported: the noise floor and the soft toe " +
                            "keep the darkest sky from being crushed.",
                            100*r.sigmaMax/r.skyL, 100*HK_STRETCHED_NOISE));
}

// Measures the view and returns the model with the given settings.
function hkAnalyze(view, amount, neutralize, target) {
   return hkModel(hkMeasureSky(view.image), amount, neutralize, target);
}

// PixelMath expression for one channel: subtract the offset, soft toe below eps.
function hkExpression(offset, eps) {
   var u = format("($T - (%.12f))", offset);
   var e = format("%.12f", eps);
   return "iif(" + u + " >= " + e + ", " + u + ", " + e + "*exp(" + u + "/" + e + " - 1))";
}

// Measures and corrects the view; returns the model used.
function hkApply(view, amount, neutralize, target) {
   var r = hkAnalyze(view, amount, neutralize, target);
   hkReport(r);

   var P = new PixelMath;
   P.useSingleExpression = r.sky.length != 3;
   P.expression = hkExpression(r.offset[0], r.eps);
   if (r.sky.length == 3) {
      P.expression1 = hkExpression(r.offset[1], r.eps);
      P.expression2 = hkExpression(r.offset[2], r.eps);
   }
   P.rescale        = false;
   P.truncate       = true;   // [0,1]; the soft toe keeps dark pixels off zero
   P.createNewImage = false;

   console.writeln("PixelMath:");
   console.writeln("   " + hkLabels(r.sky.length)[0] + ": " + P.expression);
   if (r.sky.length == 3) {
      console.writeln("   G: " + P.expression1);
      console.writeln("   B: " + P.expression2);
   }
   P.executeOn(view);
   console.noteln("Haze removed. (Ctrl+Z to undo)");
   return r;
}

// ============================================================================
// Dialog
// ============================================================================
function HazeKillDialog() {
   this.__base__ = Dialog;
   this.__base__();
   var dialog = this;

   this.title_Lbl = new Label(this);
   this.title_Lbl.frameStyle = FrameStyle_Box;
   this.title_Lbl.margin = 6;
   this.title_Lbl.useRichText = true;
   this.title_Lbl.text = "<b>" + TITLE + " " + VERSION + "</b>";
   this.title_Lbl.textAlignment = TextAlign_Center;

   this.instructions_Lbl = new TextBox(this);
   this.instructions_Lbl.readOnly = true;
   this.instructions_Lbl.frameStyle = FrameStyle_Box;
   this.instructions_Lbl.text =
      "Run AFTER gradient extraction (GraXpert / DBE). Linear or stretched images.\n\n" +
      "Measure  - reports the sky level, noise and color cast of each channel and what Apply would subtract.\n" +
      "Apply    - removes the color cast and part of the sky pedestal (undoable).\n\n" +
      "The sky is measured on the darkest 10% of 64 px tiles. It is never raised, never pushed below " +
      "4 sigma of its own noise, and dark pixels roll off smoothly instead of clipping to zero.";
   this.instructions_Lbl.setScaledMinWidth(400);
   this.instructions_Lbl.setScaledMinHeight(130);

   // target view selector
   this.viewList = new ViewList(this);
   this.viewList.getMainViews();
   if (parameters.targetView && parameters.targetView.isView)
      this.viewList.currentView = parameters.targetView;
   else
      parameters.targetView = this.viewList.currentView;
   this.viewList.onViewSelected = function(view) {
      parameters.targetView = view;
   };

   this.view_Lbl = new Label(this);
   this.view_Lbl.text = "Target image:";
   this.view_Lbl.textAlignment = TextAlign_Left | TextAlign_VertCenter;

   this.viewSizer = new HorizontalSizer;
   this.viewSizer.spacing = 6;
   this.viewSizer.add(this.view_Lbl);
   this.viewSizer.add(this.viewList, 100);

   var labelWidth = this.font.width("Neutralize:M");

   this.amount_NC = new NumericControl(this);
   this.amount_NC.label.text = "Amount:";
   this.amount_NC.label.minWidth = labelWidth;
   this.amount_NC.setRange(0.0, 1.0);
   this.amount_NC.slider.setRange(0, 100);
   this.amount_NC.setPrecision(2);
   this.amount_NC.setValue(parameters.amount);
   this.amount_NC.toolTip = "Fraction of the sky pedestal above the target that is removed " +
                            "(0 = keep it, 1 = bring the sky down to the target; default 0.5).";
   this.amount_NC.onValueUpdated = function(value) {
      parameters.amount = value;
   };

   this.neutralize_NC = new NumericControl(this);
   this.neutralize_NC.label.text = "Neutralize:";
   this.neutralize_NC.label.minWidth = labelWidth;
   this.neutralize_NC.setRange(0.0, 1.0);
   this.neutralize_NC.slider.setRange(0, 100);
   this.neutralize_NC.setPrecision(2);
   this.neutralize_NC.setValue(parameters.neutralize);
   this.neutralize_NC.toolTip = "Fraction of the sky color cast that is removed " +
                                "(0 = keep it, 1 = neutral gray sky; default 1.0).";
   this.neutralize_NC.onValueUpdated = function(value) {
      parameters.neutralize = value;
   };

   this.level_NC = new NumericControl(this);
   this.level_NC.label.text = "Target:";
   this.level_NC.label.minWidth = labelWidth;
   this.level_NC.setRange(0.0, 0.5);
   this.level_NC.slider.setRange(0, 500);
   this.level_NC.setPrecision(3);
   this.level_NC.setValue(parameters.targetLevel);
   this.level_NC.toolTip = "Lowest level the sky is brought down to (default 0.05). A sky already below " +
                           "it is left where it is; 4 sigma of the sky noise is used when that is higher.";
   this.level_NC.onValueUpdated = function(value) {
      parameters.targetLevel = value;
   };

   // buttons
   this.newInstance_Btn = new ToolButton(this);
   this.newInstance_Btn.icon = this.scaledResource(":/process-interface/new-instance.png");
   this.newInstance_Btn.setScaledFixedSize(24, 24);
   this.newInstance_Btn.toolTip = "Save current parameters as a process icon.";
   this.newInstance_Btn.onMousePress = function() {
      parameters.save();
      dialog.newInstance();
   };

   this.measure_Btn = new PushButton(this);
   this.measure_Btn.text = "Measure";
   this.measure_Btn.toolTip = "Report the sky, noise and cast and what Apply would subtract, without changing the image.";
   this.measure_Btn.onClick = function() {
      var view = parameters.targetView;
      if (!view || !view.isView) { console.criticalln("No target image selected."); return; }
      console.show();
      console.noteln("== " + TITLE + " : measure ==");
      hkReport(hkAnalyze(view, parameters.amount, parameters.neutralize, parameters.targetLevel));
   };

   this.apply_Btn = new PushButton(this);
   this.apply_Btn.text = "Apply";
   this.apply_Btn.toolTip = "Remove the haze from the target image.";
   this.apply_Btn.onClick = function() {
      var view = parameters.targetView;
      if (!view || !view.isView) { console.criticalln("No target image selected."); return; }
      console.show();
      console.noteln("== " + TITLE + " : apply ==");
      hkApply(view, parameters.amount, parameters.neutralize, parameters.targetLevel);
   };

   this.close_Btn = new PushButton(this);
   this.close_Btn.text = "Close";
   this.close_Btn.onClick = function() { dialog.cancel(); };

   this.buttonSizer = new HorizontalSizer;
   this.buttonSizer.spacing = 6;
   this.buttonSizer.add(this.newInstance_Btn);
   this.buttonSizer.addStretch();
   this.buttonSizer.add(this.measure_Btn);
   this.buttonSizer.add(this.apply_Btn);
   this.buttonSizer.add(this.close_Btn);

   this.sizer = new VerticalSizer;
   this.sizer.margin = 8;
   this.sizer.spacing = 8;
   this.sizer.add(this.title_Lbl);
   this.sizer.add(this.instructions_Lbl);
   this.sizer.add(this.viewSizer);
   this.sizer.add(this.amount_NC);
   this.sizer.add(this.neutralize_NC);
   this.sizer.add(this.level_NC);
   this.sizer.addSpacing(4);
   this.sizer.add(this.buttonSizer);

   this.windowTitle = TITLE + " " + VERSION;
   this.adjustToContents();
   this.setFixedSize();
}
HazeKillDialog.prototype = new Dialog;

// ============================================================================
// Main
// ============================================================================
function main() {
   console.show();
   console.noteln(TITLE + " " + VERSION);

   if (Parameters.isViewTarget) {
      parameters.load();
      hkApply(Parameters.targetView, parameters.amount, parameters.neutralize, parameters.targetLevel);
      return;
   }

   if (ImageWindow.windows.length === 0) {
      (new MessageBox("No images are open. Please open an image first.",
         TITLE, StdIcon_Error, StdButton_Ok)).execute();
      return;
   }

   var dialog = new HazeKillDialog();
   dialog.execute();
}

// The headless parity test includes this file with EZHAZEKILL_NO_MAIN defined
// and calls hkApply() directly.
#ifndef EZHAZEKILL_NO_MAIN
main();
#endif
