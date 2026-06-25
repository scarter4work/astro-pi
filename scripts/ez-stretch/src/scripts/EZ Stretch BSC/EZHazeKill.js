// ============================================================================
// EZHazeKill.js - Flat-background haze detector + corrector
// ============================================================================
//
// Removes the flat, slightly-colored background haze that remains AFTER
// background extraction (GraXpert / DBE / GradientCorrection) on moon-washed
// or light-polluted linear frames. It measures each channel's background,
// reports the color-cast spread, and subtracts every channel down to a common
// neutral target via PixelMath (undoable, expressions visible in history).
//
// It does NOT model a gradient - run GraXpert/DBE first. This only kills a
// flat per-channel pedestal. Assumes the target object does not fill most of
// the frame (median ~ sky); a frame-filling nebula biases the median high.
//
// Copyright (c) 2026 EZ Stretch BSC
// Licensed under CC BY-NC 4.0  -  http://creativecommons.org/licenses/by-nc/4.0/
//
// ============================================================================

#feature-id    scarter4work > EZ Haze Kill
#script-id     EZHazeKill
#feature-info  Detects and removes flat background haze (residual sky pedestal \
               and color cast) left after gradient extraction.

#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/TextAlign.jsh>

#define TITLE   "EZ Haze Kill"
#define VERSION "1.0.1"

var jsAutoGC = true;

// ============================================================================
// Parameters
// ============================================================================
function HazeKillParameters() {
   this.targetView  = undefined;
   this.targetLevel = 0.05;      // neutral level all channels are moved to

   this.save = function() {
      Parameters.set("targetLevel", this.targetLevel);
   };
   this.load = function() {
      if (Parameters.has("targetLevel"))
         this.targetLevel = Parameters.getReal("targetLevel");
   };
}
var parameters = new HazeKillParameters();

// ============================================================================
// Core: measure / report / correct
// ============================================================================
function measureBackground(view) {
   var img = view.image;
   var nc  = img.isColor ? 3 : 1;
   var rect = new Rect(0, 0, img.width, img.height);
   var bg = [];
   for (var c = 0; c < nc; ++c)
      bg.push(img.median(rect, c, c));    // per-channel median = sky proxy
   return bg;
}

function reportBackground(bg, isColor) {
   var labels = isColor ? ["R", "G", "B"] : ["K"];
   console.writeln("Background levels:");
   for (var c = 0; c < bg.length; ++c)
      console.writeln(format("   %s: %.5f", labels[c], bg[c]));
   if (isColor) {
      var lo = Math.min(bg[0], bg[1], bg[2]);
      var hi = Math.max(bg[0], bg[1], bg[2]);
      console.writeln(format("   cast spread (max-min): %.5f%s", hi - lo,
                      (hi - lo) > 0.005 ? "   <-- cast present" : "   (already neutral)"));
   }
}

function applyCorrection(view, target) {
   var img = view.image;
   var bg  = measureBackground(view);
   reportBackground(bg, img.isColor);

   var P = new PixelMath;
   P.useSingleExpression = !img.isColor;
   if (img.isColor) {
      P.expression  = format("$T - %.6f + %.6f", bg[0], target);   // R
      P.expression1 = format("$T - %.6f + %.6f", bg[1], target);   // G
      P.expression2 = format("$T - %.6f + %.6f", bg[2], target);   // B
   } else {
      P.expression  = format("$T - %.6f + %.6f", bg[0], target);   // K
   }
   P.rescale        = false;
   P.truncate       = true;       // clip out-of-range to [0,1]
   P.createNewImage = false;

   console.writeln("PixelMath:");
   console.writeln("   R/K: " + P.expression);
   if (img.isColor) {
      console.writeln("   G:   " + P.expression1);
      console.writeln("   B:   " + P.expression2);
   }
   P.executeOn(view);
   console.noteln(format("Haze removed - background neutralized to %.3f. (Ctrl+Z to undo)", target));
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
      "Run on a LINEAR image, AFTER gradient extraction (GraXpert / DBE).\n\n" +
      "Measure  - reports each channel's background level and the color-cast spread.\n" +
      "Apply    - subtracts every channel to the target level (neutral, undoable).\n\n" +
      "Only removes a flat pedestal + color cast. It does not fix a gradient.";
   this.instructions_Lbl.setScaledMinWidth(360);
   this.instructions_Lbl.setScaledMinHeight(110);

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

   // target level numeric control
   this.level_NC = new NumericControl(this);
   this.level_NC.label.text = "Target level:";
   this.level_NC.label.minWidth = 80;
   this.level_NC.setRange(0.0, 0.5);
   this.level_NC.slider.setRange(0, 500);
   this.level_NC.setPrecision(3);
   this.level_NC.setValue(parameters.targetLevel);
   this.level_NC.toolTip = "Neutral background level all channels are moved to (default 0.05).";
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
   this.measure_Btn.toolTip = "Report background levels and cast without changing the image.";
   this.measure_Btn.onClick = function() {
      var view = parameters.targetView;
      if (!view || !view.isView) { console.criticalln("No target image selected."); return; }
      console.show();
      console.noteln("== " + TITLE + " : measure ==");
      reportBackground(measureBackground(view), view.image.isColor);
   };

   this.apply_Btn = new PushButton(this);
   this.apply_Btn.text = "Apply";
   this.apply_Btn.toolTip = "Subtract the haze and neutralize the background.";
   this.apply_Btn.onClick = function() {
      var view = parameters.targetView;
      if (!view || !view.isView) { console.criticalln("No target image selected."); return; }
      console.show();
      console.noteln("== " + TITLE + " : apply ==");
      applyCorrection(view, parameters.targetLevel);
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
      applyCorrection(Parameters.targetView, parameters.targetLevel);
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

main();
