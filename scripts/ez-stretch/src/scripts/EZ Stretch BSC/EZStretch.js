// ============================================================================
// EZStretch.js - Unified Stretch Suite for PixInsight
// ============================================================================
//
// Combines three stretch algorithms into a single interface:
// - Lupton RGB: Color-preserving asinh stretch (Lupton et al. 2004)
// - Root-power stretch: each channel raised to the power 1/rootpower (optional second pass, S-curve, saturation)
// - PhotometricStretch: luminance-weighted asinh stretch (Rec. 709 weights)
//
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE in the astro-pi repository)
//
// Provenance: the root-power stretch is an independent implementation, written
// from the author's own requirements, inspired by the published idea of
// power/root stretching of astronomical images. Power stretching in general is
// described publicly by Roger N. Clark (clarkvision.com). This is not Clark's
// software, contains none of its code, and is not affiliated with or endorsed
// by him. It does no colour recovery.
//
// ============================================================================

// The "Id : menu path" form names the script EZStretch (so PixInsight finds
// its documentation at doc/scripts/EZStretch/EZStretch.html) and replaces
// #script-id: code signing allows only one identification directive.
#feature-id    EZStretch : scarter4work > EZ Stretch
#feature-info  Unified stretch suite combining Lupton RGB, a root-power stretch, \
               and PhotometricStretch into a single interface. Select your \
               algorithm and adjust parameters with real-time preview.

#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/TextAlign.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/StdCursor.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/UndoFlag.jsh>
#include <pjsr/SampleType.jsh>
#include <pjsr/Color.jsh>
#include <pjsr/ImageOp.jsh>
#include <pjsr/ButtonCodes.jsh>

#iflt __PI_VERSION__ 01.08.00
#error This script requires PixInsight 1.8.0 or higher.
#endif

#define VERSION "1.0.12"
#define TITLE   "EZ Stretch"

var jsAutoGC = true;

// Math.asinh polyfill
if (typeof Math.asinh === 'undefined') {
   Math.asinh = function(x) {
      return Math.log(x + Math.sqrt(x * x + 1));
   };
}

// ============================================================================
// Algorithm Constants
// ============================================================================

var Algorithm = {
   LUPTON: 0,
   ROOTPOWER: 1,
   PHOTOMETRIC: 2
};

var AlgorithmNames = ["Lupton RGB", "Root-power stretch", "PhotometricStretch"];

// ============================================================================
// Lupton Engine (from LuptonRGB.js)
// ============================================================================

function LuptonEngine() {
   this.stretch = 5.0;
   this.Q = 8.0;
   this.blackPoint = 0.0;
   this.saturation = 1.0;
   this.clippingMode = 0; // 0: Preserve Color, 1: Hard Clip, 2: Rescale

   this.F = function(x, alpha, Q, minimum) {
      var val = x - minimum;
      if (val <= 0) return 0;
      return Math.asinh(alpha * Q * val) / Q;
   };

   this.processPixel = function(r, g, b) {
      var minimum = this.blackPoint;
      var I = (r + g + b) / 3;
      var scale = 0;
      var epsilon = 1e-10;

      if (I > minimum + epsilon) {
         var FI = this.F(I, this.stretch, this.Q, minimum);
         scale = FI / (I - minimum);
      }

      var rOut = (r - minimum) * scale;
      var gOut = (g - minimum) * scale;
      var bOut = (b - minimum) * scale;

      if (Math.abs(this.saturation - 1.0) > 1e-6) {
         var lum = (rOut + gOut + bOut) / 3;
         rOut = lum + (rOut - lum) * this.saturation;
         gOut = lum + (gOut - lum) * this.saturation;
         bOut = lum + (bOut - lum) * this.saturation;
      }

      if (this.clippingMode === 0) {
         var maxVal = Math.max(rOut, gOut, bOut);
         if (maxVal > 1.0) {
            rOut /= maxVal;
            gOut /= maxVal;
            bOut /= maxVal;
         }
      } else if (this.clippingMode === 1) {
         rOut = Math.min(1.0, Math.max(0, rOut));
         gOut = Math.min(1.0, Math.max(0, gOut));
         bOut = Math.min(1.0, Math.max(0, bOut));
      }

      return [Math.max(0, rOut), Math.max(0, gOut), Math.max(0, bOut)];
   };

   this.execute = function(targetWindow) {
      if (!targetWindow) return null;

      var sourceImage = targetWindow.mainView.image;
      if (sourceImage.numberOfChannels < 3) return null;

      var width = sourceImage.width;
      var height = sourceImage.height;
      var outputId = targetWindow.mainView.id + "_lupton";

      var outputWindow = new ImageWindow(width, height, 3, 32, true, true, outputId);

      outputWindow.mainView.beginProcess(UndoFlag_NoSwapFile);
      try {
         outputWindow.mainView.image.apply(sourceImage);
      } finally {
         outputWindow.mainView.endProcess();
      }

      var alpha = this.stretch;
      var Q = this.Q;
      var minVal = this.blackPoint;
      var safeQ = (Math.abs(Q) < 0.01) ? 0.01 : Q;
      var aQ = alpha * safeQ;
      var epsilon = 1e-10;

      var intensity = "($T[0]+$T[1]+$T[2])/3";
      var arg = aQ + "*(" + intensity + "-" + minVal + ")";
      var FI = "ln(" + arg + "+sqrt(" + arg + "*" + arg + "+1))/" + safeQ;
      var scale = "iif(" + intensity + ">" + (minVal + epsilon) + "," + FI + "/max(" + epsilon + "," + intensity + "-" + minVal + "),0)";

      outputWindow.mainView.beginProcess(UndoFlag_NoSwapFile);
      try {
         var P1 = new PixelMath;
         P1.expression = "max(0,($T[0]-" + minVal + ")*" + scale + ")";
         P1.expression1 = "max(0,($T[1]-" + minVal + ")*" + scale + ")";
         P1.expression2 = "max(0,($T[2]-" + minVal + ")*" + scale + ")";
         P1.useSingleExpression = false;
         P1.createNewImage = false;
         P1.rescale = false;
         P1.truncate = false;
         P1.executeOn(outputWindow.mainView);

         if (Math.abs(this.saturation - 1.0) > 1e-6) {
            var sat = this.saturation;
            var P2 = new PixelMath;
            var lum = "($T[0]+$T[1]+$T[2])/3";
            P2.expression = lum + "+($T[0]-" + lum + ")*" + sat;
            P2.expression1 = lum + "+($T[1]-" + lum + ")*" + sat;
            P2.expression2 = lum + "+($T[2]-" + lum + ")*" + sat;
            P2.useSingleExpression = false;
            P2.createNewImage = false;
            P2.rescale = false;
            P2.truncate = false;
            P2.executeOn(outputWindow.mainView);
         }

         var P3 = new PixelMath;
         if (this.clippingMode === 0) {
            var maxRGB = "max($T[0],max($T[1],$T[2]))";
            var clipScale = "iif(" + maxRGB + ">1,1/" + maxRGB + ",1)";
            P3.expression = "max(0,$T[0]*" + clipScale + ")";
            P3.expression1 = "max(0,$T[1]*" + clipScale + ")";
            P3.expression2 = "max(0,$T[2]*" + clipScale + ")";
            P3.rescale = false;
         } else if (this.clippingMode === 1) {
            P3.expression = "min(1,max(0,$T[0]))";
            P3.expression1 = "min(1,max(0,$T[1]))";
            P3.expression2 = "min(1,max(0,$T[2]))";
            P3.rescale = false;
         } else {
            P3.expression = "$T[0]";
            P3.expression1 = "$T[1]";
            P3.expression2 = "$T[2]";
            P3.rescale = true;
         }
         P3.useSingleExpression = false;
         P3.createNewImage = false;
         P3.truncate = true;
         P3.executeOn(outputWindow.mainView);
      } finally {
         outputWindow.mainView.endProcess();
      }

      outputWindow.show();
      return outputWindow;
   };

   this.reset = function() {
      this.stretch = 5.0;
      this.Q = 8.0;
      this.blackPoint = 0.0;
      this.saturation = 1.0;
      this.clippingMode = 0;
   };
}

// ============================================================================
// Root-power Engine
// ============================================================================

function RootPowerEngine() {
   this.rootpower = 6.0;
   this.rootpower2 = 3.0;
   this.twoPass = false;
   this.scurve = 0;
   this.enhance = 1.0;

   this.processPixel = function(r, g, b) {
      var exponent = 1.0 / this.rootpower;
      r = Math.pow(Math.max(0, r), exponent);
      g = Math.pow(Math.max(0, g), exponent);
      b = Math.pow(Math.max(0, b), exponent);

      if (this.twoPass && this.rootpower2 > 1) {
         exponent = 1.0 / this.rootpower2;
         r = Math.pow(r, exponent);
         g = Math.pow(g, exponent);
         b = Math.pow(b, exponent);
      }

      if (this.scurve > 0) {
         r = this.applySCurveValue(r, this.scurve);
         g = this.applySCurveValue(g, this.scurve);
         b = this.applySCurveValue(b, this.scurve);
      }

      if (this.enhance != 1.0) {
         var lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
         r = lum + (r - lum) * this.enhance;
         g = lum + (g - lum) * this.enhance;
         b = lum + (b - lum) * this.enhance;
      }

      return [
         Math.max(0, Math.min(1, r)),
         Math.max(0, Math.min(1, g)),
         Math.max(0, Math.min(1, b))
      ];
   };

   this.applySCurveValue = function(x, curveType) {
      var y;
      switch (curveType) {
         case 1:
            y = x + 0.15 * Math.sin(Math.PI * x) * (1 - x);
            break;
         case 2:
            y = Math.pow(x, 0.85);
            break;
         case 3:
            var midpoint = 0.5;
            y = x + 0.1 * Math.sin(Math.PI * x * 2) * Math.exp(-Math.pow((x - midpoint) / 0.3, 2));
            break;
         case 4:
            var t = x * x * (3 - 2 * x);
            y = x * 0.3 + t * 0.7;
            break;
         default:
            y = x;
      }
      return Math.max(0, Math.min(1, y));
   };

   this.execute = function(targetWindow) {
      if (!targetWindow) return null;

      var sourceImage = targetWindow.mainView.image;
      if (sourceImage.numberOfChannels < 3) return null;

      var width = sourceImage.width;
      var height = sourceImage.height;
      var outputId = targetWindow.mainView.id + "_rootpower";

      var outputWindow = new ImageWindow(width, height, 3, 32, true, true, outputId);
      var newView = outputWindow.mainView;

      newView.beginProcess(UndoFlag_NoSwapFile);
      try {
         newView.image.assign(sourceImage);
         this.applyFull(newView.image);
      } finally {
         newView.endProcess();
      }

      outputWindow.show();
      return outputWindow;
   };

   this.applyFull = function(image) {
      // Power stretch
      if (this.rootpower > 1) {
         image.apply(1.0 / this.rootpower, ImageOp_Pow);
      }

      // Second pass
      if (this.twoPass && this.rootpower2 > 1) {
         image.apply(1.0 / this.rootpower2, ImageOp_Pow);
      }

      // S-curve
      if (this.scurve > 0) {
         this.applySCurveImage(image, this.scurve);
      }

      // Saturation
      if (this.enhance != 1.0) {
         this.applySaturationImage(image, this.enhance);
      }

      image.truncate(0, 1);
   };

   this.applySCurveImage = function(image, curveType) {
      var width = image.width;
      var height = image.height;
      var rect = new Rect(0, 0, width, height);

      for (var c = 0; c < 3; c++) {
         var samples = [];
         image.getSamples(samples, rect, c);
         for (var i = 0; i < samples.length; i++) {
            samples[i] = this.applySCurveValue(samples[i], curveType);
         }
         image.setSamples(samples, rect, c);
      }
   };

   this.applySaturationImage = function(image, factor) {
      var width = image.width;
      var height = image.height;
      var rect = new Rect(0, 0, width, height);

      var R = [], G = [], B = [];
      image.getSamples(R, rect, 0);
      image.getSamples(G, rect, 1);
      image.getSamples(B, rect, 2);

      for (var i = 0; i < R.length; i++) {
         var lum = 0.2126 * R[i] + 0.7152 * G[i] + 0.0722 * B[i];
         R[i] = lum + (R[i] - lum) * factor;
         G[i] = lum + (G[i] - lum) * factor;
         B[i] = lum + (B[i] - lum) * factor;
      }

      image.setSamples(R, rect, 0);
      image.setSamples(G, rect, 1);
      image.setSamples(B, rect, 2);
   };

   this.reset = function() {
      this.rootpower = 6.0;
      this.rootpower2 = 3.0;
      this.twoPass = false;
      this.scurve = 0;
      this.enhance = 1.0;
   };
}

// ============================================================================
// Photometric Engine (simplified)
// ============================================================================

function PhotometricEngine() {
   this.Q = 100.0;           // Stretch (alpha)
   this.softening = 25.0;    // Q parameter
   this.blackPoint = 0.0;
   this.saturation = 1.0;
   this.weights = { R: 0.2126, G: 0.7152, B: 0.0722 };

   this.processPixel = function(r, g, b) {
      r = Math.max(0, r - this.blackPoint);
      g = Math.max(0, g - this.blackPoint);
      b = Math.max(0, b - this.blackPoint);

      var I = this.weights.R * r + this.weights.G * g + this.weights.B * b;

      if (I < 1e-10) {
         return [0, 0, 0];
      }

      var soft = Math.max(0.01, this.softening);
      var normFactor = 1.0 / Math.asinh(this.Q / soft);
      var stretchedI = Math.asinh(this.Q * I / soft) * normFactor;
      var scaleFactor = stretchedI / I;

      var rOut = r * scaleFactor;
      var gOut = g * scaleFactor;
      var bOut = b * scaleFactor;

      if (this.saturation !== 1.0) {
         var lum = this.weights.R * rOut + this.weights.G * gOut + this.weights.B * bOut;
         rOut = lum + this.saturation * (rOut - lum);
         gOut = lum + this.saturation * (gOut - lum);
         bOut = lum + this.saturation * (bOut - lum);
      }

      return [
         Math.max(0, Math.min(1, rOut)),
         Math.max(0, Math.min(1, gOut)),
         Math.max(0, Math.min(1, bOut))
      ];
   };

   this.execute = function(targetWindow) {
      if (!targetWindow) return null;

      var sourceImage = targetWindow.mainView.image;
      if (sourceImage.numberOfChannels < 3) return null;

      var width = sourceImage.width;
      var height = sourceImage.height;
      var outputId = targetWindow.mainView.id + "_photo";

      var outputWindow = new ImageWindow(width, height, 3, 32, true, true, outputId);
      var newView = outputWindow.mainView;

      newView.beginProcess(UndoFlag_NoSwapFile);
      try {
         newView.image.assign(sourceImage);
         this.applyFull(newView.image);
      } finally {
         newView.endProcess();
      }

      outputWindow.show();
      return outputWindow;
   };

   this.applyFull = function(image) {
      var width = image.width;
      var height = image.height;
      var rect = new Rect(0, 0, width, height);

      var R = [], G = [], B = [];
      image.getSamples(R, rect, 0);
      image.getSamples(G, rect, 1);
      image.getSamples(B, rect, 2);

      for (var i = 0; i < R.length; i++) {
         var result = this.processPixel(R[i], G[i], B[i]);
         R[i] = result[0];
         G[i] = result[1];
         B[i] = result[2];
      }

      image.setSamples(R, rect, 0);
      image.setSamples(G, rect, 1);
      image.setSamples(B, rect, 2);
   };

   this.reset = function() {
      this.Q = 100.0;
      this.softening = 25.0;
      this.blackPoint = 0.0;
      this.saturation = 1.0;
      this.weights = { R: 0.2126, G: 0.7152, B: 0.0722 };
   };
}

// ============================================================================
// Unified Preview Control
// ============================================================================

function UnifiedPreviewControl(parent) {
   this.__base__ = Frame;
   this.__base__(parent);

   this.algorithm = Algorithm.LUPTON;
   this.luptonEngine = new LuptonEngine();
   this.rpEngine = new RootPowerEngine();
   this.photoEngine = new PhotometricEngine();

   this.scaledImage = null;
   this.sourceWindow = null;
   this.previewMode = 0;  // 0: After, 1: Before, 2: Split
   this.splitPosition = 50;
   this.zoomFit = true;   // true = fit to window, false = use zoomScale
   this.zoomScale = 1.0;  // actual zoom scale when not fitting
   this.scale = 1.0;      // current effective scale
   this.scrollX = 0;
   this.scrollY = 0;
   this.dragging = false;
   this.dragStartX = 0;
   this.dragStartY = 0;
   this.onZoomChanged = null;  // callback when zoom changes

   var self = this;

   this.scrollbox = new ScrollBox(this);
   this.scrollbox.autoScroll = true;
   this.scrollbox.tracking = true;

   this.scrollbox.viewport.onPaint = function(x0, y0, x1, y1) {
      var graphics = new VectorGraphics(this);
      graphics.fillRect(x0, y0, x1, y1, new Brush(0xff202020));

      if (self.scaledImage) {
         var offsetX = (this.width - self.scaledImage.width) / 2;
         var offsetY = (this.height - self.scaledImage.height) / 2;
         if (offsetX < 0) offsetX = -self.scrollbox.horizontalScrollPosition;
         if (offsetY < 0) offsetY = -self.scrollbox.verticalScrollPosition;

         graphics.translateTransformation(offsetX, offsetY);
         graphics.drawBitmap(0, 0, self.scaledImage);

         graphics.pen = new Pen(0xffffffff, 0);
         graphics.drawRect(-1, -1, self.scaledImage.width + 1, self.scaledImage.height + 1);

         if (self.previewMode === 2) {
            var splitX = Math.round(self.scaledImage.width * self.splitPosition / 100);
            graphics.pen = new Pen(0xaaffffff, 2);
            graphics.drawLine(splitX, 0, splitX, self.scaledImage.height);
         }

         graphics.antialiasing = true;
         graphics.pen = new Pen(0xffffffff);
         var algName = AlgorithmNames[self.algorithm];
         if (self.previewMode === 2) {
            graphics.drawText(5, 15, "BEFORE");
            graphics.drawText(self.scaledImage.width - 50, 15, algName);
         } else if (self.previewMode === 1) {
            graphics.drawText(5, 15, "BEFORE (Linear)");
         } else {
            graphics.drawText(5, 15, algName);
         }
      } else {
         graphics.pen = new Pen(0xff888888);
         graphics.drawText(this.width / 2 - 50, this.height / 2, "No image loaded");
      }

      graphics.end();
   };

   this.scrollbox.viewport.onResize = function(wNew, hNew, wOld, hOld) {
      self.regenerate();
   };

   // Mouse drag for panning when zoomed
   this.scrollbox.viewport.onMousePress = function(x, y, button, modifiers) {
      if (button === MouseButton_Left) {
         self.dragging = true;
         self.dragStartX = x;
         self.dragStartY = y;
         this.cursor = new Cursor(StdCursor_ClosedHand);
      }
   };

   this.scrollbox.viewport.onMouseRelease = function(x, y, button, modifiers) {
      if (button === MouseButton_Left) {
         self.dragging = false;
         this.cursor = new Cursor(StdCursor_OpenHand);
      }
   };

   this.scrollbox.viewport.onMouseMove = function(x, y, modifiers) {
      if (self.dragging) {
         var dx = self.dragStartX - x;
         var dy = self.dragStartY - y;
         self.scrollbox.horizontalScrollPosition += dx;
         self.scrollbox.verticalScrollPosition += dy;
         self.dragStartX = x;
         self.dragStartY = y;
      }
   };

   // Mouse wheel zoom
   this.scrollbox.viewport.onMouseWheel = function(x, y, delta, buttonState, modifiers) {
      if (delta > 0) {
         self.zoomIn();
      } else if (delta < 0) {
         self.zoomOut();
      }
   };

   this.sizer = new VerticalSizer;
   this.sizer.add(this.scrollbox);
   this.setScaledMinSize(350, 280);

   this.setAlgorithm = function(alg) {
      this.algorithm = alg;
      this.regenerate();
   };

   // Zoom in by a factor
   this.zoomIn = function() {
      if (this.zoomFit) {
         // Start from current fit scale
         this.zoomScale = this.scale * 1.25;
         this.zoomFit = false;
      } else {
         this.zoomScale = Math.min(8.0, this.zoomScale * 1.25);
      }
      this.regenerate();
      if (this.onZoomChanged) this.onZoomChanged();
   };

   // Zoom out by a factor
   this.zoomOut = function() {
      if (this.zoomFit) {
         // Start from current fit scale
         this.zoomScale = this.scale / 1.25;
         this.zoomFit = false;
      } else {
         this.zoomScale = Math.max(0.1, this.zoomScale / 1.25);
      }
      this.regenerate();
      if (this.onZoomChanged) this.onZoomChanged();
   };

   // Fit to window
   this.zoomToFit = function() {
      this.zoomFit = true;
      this.regenerate();
      if (this.onZoomChanged) this.onZoomChanged();
   };

   // Get current zoom percentage for display
   this.getZoomPercent = function() {
      return Math.round(this.scale * 100);
   };

   this.updatePreview = function() {
      this.regenerate();
   };

   this.regenerate = function() {
      this.scaledImage = null;
      if (!this.sourceWindow) {
         this.scrollbox.viewport.update();
         return;
      }

      var image = this.sourceWindow.mainView.image;
      if (!image || image.numberOfChannels < 3) {
         this.scrollbox.viewport.update();
         return;
      }

      var vpWidth = this.scrollbox.viewport.width;
      var vpHeight = this.scrollbox.viewport.height;
      if (vpWidth <= 0 || vpHeight <= 0) return;

      var imgWidth = image.width;
      var imgHeight = image.height;

      if (this.zoomFit) {
         // Fit to viewport
         var scaleX = (vpWidth - 10) / imgWidth;
         var scaleY = (vpHeight - 10) / imgHeight;
         this.scale = Math.min(scaleX, scaleY);
      } else {
         this.scale = this.zoomScale;
      }

      var outWidth = Math.max(1, Math.round(imgWidth * this.scale));
      var outHeight = Math.max(1, Math.round(imgHeight * this.scale));

      this.scaledImage = this.generatePreviewBitmap(outWidth, outHeight);

      if (this.scaledImage) {
         this.scrollbox.maxHorizontalScrollPosition = Math.max(0, this.scaledImage.width - vpWidth);
         this.scrollbox.maxVerticalScrollPosition = Math.max(0, this.scaledImage.height - vpHeight);
      }

      // Set cursor for pan capability
      if (this.scale > 0 && (outWidth > vpWidth || outHeight > vpHeight)) {
         this.scrollbox.viewport.cursor = new Cursor(StdCursor_OpenHand);
      } else {
         this.scrollbox.viewport.cursor = new Cursor(StdCursor_Arrow);
      }

      this.scrollbox.viewport.update();
   };

   this.generatePreviewBitmap = function(outWidth, outHeight) {
      if (!this.sourceWindow) return null;

      var image = this.sourceWindow.mainView.image;
      var imgWidth = image.width;
      var imgHeight = image.height;

      var bitmap = new Bitmap(outWidth, outHeight);
      var scaleX = imgWidth / outWidth;
      var scaleY = imgHeight / outHeight;
      var splitX = outWidth * this.splitPosition / 100;

      var engine;
      switch (this.algorithm) {
         case Algorithm.LUPTON:
            engine = this.luptonEngine;
            break;
         case Algorithm.ROOTPOWER:
            engine = this.rpEngine;
            break;
         case Algorithm.PHOTOMETRIC:
            engine = this.photoEngine;
            break;
      }

      for (var py = 0; py < outHeight; py++) {
         var iy = Math.min(Math.floor(py * scaleY), imgHeight - 1);

         for (var px = 0; px < outWidth; px++) {
            var ix = Math.min(Math.floor(px * scaleX), imgWidth - 1);

            var r = image.sample(ix, iy, 0);
            var g = image.sample(ix, iy, 1);
            var b = image.sample(ix, iy, 2);

            var rOut, gOut, bOut;
            var isBefore = (this.previewMode === 1) || (this.previewMode === 2 && px < splitX);

            if (isBefore) {
               rOut = Math.min(1, r * 10);
               gOut = Math.min(1, g * 10);
               bOut = Math.min(1, b * 10);
            } else {
               var result = engine.processPixel(r, g, b);
               rOut = result[0];
               gOut = result[1];
               bOut = result[2];
            }

            var r8 = (rOut > 1 ? 255 : (rOut < 0 ? 0 : (rOut * 255 + 0.5) | 0));
            var g8 = (gOut > 1 ? 255 : (gOut < 0 ? 0 : (gOut * 255 + 0.5) | 0));
            var b8 = (bOut > 1 ? 255 : (bOut < 0 ? 0 : (bOut * 255 + 0.5) | 0));

            bitmap.setPixel(px, py, 0xff000000 | (r8 << 16) | (g8 << 8) | b8);
         }
      }

      return bitmap;
   };
}

UnifiedPreviewControl.prototype = new Frame;

// ============================================================================
// Main Dialog
// ============================================================================

function EZStretchDialog() {
   this.__base__ = Dialog;
   this.__base__();

   var dialog = this;

   this.algorithm = Algorithm.LUPTON;
   this.targetWindow = null;

   this.luptonEngine = new LuptonEngine();
   this.rpEngine = new RootPowerEngine();
   this.photoEngine = new PhotometricEngine();

   this.windowTitle = TITLE + " v" + VERSION;
   this.userResizable = true;

   // Throttling
   this.lastPreviewTime = 0;
   this.previewSkipCount = 0;

   // =========================================================================
   // Target Image Section
   // =========================================================================

   this.targetLabel = new Label(this);
   this.targetLabel.text = "Target:";
   this.targetLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.targetLabel.setFixedWidth(60);

   this.targetCombo = new ComboBox(this);
   this.targetCombo.toolTip = "Select target RGB image";
   this.targetCombo.onItemSelected = function(index) {
      if (index > 0) {
         var windows = ImageWindow.windows;
         if (index - 1 < windows.length) {
            dialog.targetWindow = windows[index - 1];
            dialog.previewControl.sourceWindow = dialog.targetWindow;
            dialog.schedulePreviewUpdate();
         }
      }
   };

   this.populateImageList = function() {
      var windows = ImageWindow.windows;
      this.targetCombo.clear();
      this.targetCombo.addItem("<select image>");

      for (var i = 0; i < windows.length; i++) {
         this.targetCombo.addItem(windows[i].mainView.id);
      }

      if (ImageWindow.activeWindow && !ImageWindow.activeWindow.isNull) {
         this.targetWindow = ImageWindow.activeWindow;
         for (var i = 0; i < windows.length; i++) {
            if (windows[i].mainView.id === this.targetWindow.mainView.id) {
               this.targetCombo.currentItem = i + 1;
               break;
            }
         }
      }
   };

   var targetSizer = new HorizontalSizer;
   targetSizer.spacing = 4;
   targetSizer.add(this.targetLabel);
   targetSizer.add(this.targetCombo, 100);

   // =========================================================================
   // Algorithm Selection
   // =========================================================================

   this.algorithmLabel = new Label(this);
   this.algorithmLabel.text = "Algorithm:";
   this.algorithmLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.algorithmLabel.setFixedWidth(60);

   this.algorithmCombo = new ComboBox(this);
   this.algorithmCombo.addItem("Lupton RGB - Color-preserving asinh stretch");
   this.algorithmCombo.addItem("Root-power stretch");
   this.algorithmCombo.addItem("PhotometricStretch - Luminance-weighted asinh stretch");
   this.algorithmCombo.currentItem = 0;
   this.algorithmCombo.toolTip = "Select stretch algorithm";
   this.algorithmCombo.onItemSelected = function(index) {
      dialog.algorithm = index;
      dialog.updateParameterPanels();
      dialog.previewControl.setAlgorithm(index);
      dialog.schedulePreviewUpdate();
   };

   var algorithmSizer = new HorizontalSizer;
   algorithmSizer.spacing = 4;
   algorithmSizer.add(this.algorithmLabel);
   algorithmSizer.add(this.algorithmCombo, 100);

   this.inputGroup = new GroupBox(this);
   this.inputGroup.title = "Input";
   this.inputGroup.sizer = new VerticalSizer;
   this.inputGroup.sizer.margin = 6;
   this.inputGroup.sizer.spacing = 4;
   this.inputGroup.sizer.add(targetSizer);
   this.inputGroup.sizer.add(algorithmSizer);

   // =========================================================================
   // Lupton Parameters Panel
   // =========================================================================

   this.luptonStretchControl = new NumericControl(this);
   this.luptonStretchControl.label.text = "Stretch (\u03B1):";
   this.luptonStretchControl.label.setFixedWidth(80);
   this.luptonStretchControl.setRange(0.1, 1000.0);
   this.luptonStretchControl.slider.setRange(0, 10000);
   this.luptonStretchControl.setPrecision(2);
   this.luptonStretchControl.setValue(this.luptonEngine.stretch);
   this.luptonStretchControl.toolTip = "Linear amplification factor";
   this.luptonStretchControl.onValueUpdated = function(value) {
      dialog.luptonEngine.stretch = value;
      dialog.previewControl.luptonEngine.stretch = value;
      dialog.schedulePreviewUpdate();
   };

   this.luptonQControl = new NumericControl(this);
   this.luptonQControl.label.text = "Q (softening):";
   this.luptonQControl.label.setFixedWidth(80);
   this.luptonQControl.setRange(-10.0, 30.0);
   this.luptonQControl.slider.setRange(0, 4000);
   this.luptonQControl.setPrecision(2);
   this.luptonQControl.setValue(this.luptonEngine.Q);
   this.luptonQControl.toolTip = "Controls linear-to-log transition";
   this.luptonQControl.onValueUpdated = function(value) {
      dialog.luptonEngine.Q = value;
      dialog.previewControl.luptonEngine.Q = value;
      dialog.schedulePreviewUpdate();
   };

   this.luptonBlackControl = new NumericControl(this);
   this.luptonBlackControl.label.text = "Black Point:";
   this.luptonBlackControl.label.setFixedWidth(80);
   this.luptonBlackControl.setRange(0, 0.1);
   this.luptonBlackControl.slider.setRange(0, 1000);
   this.luptonBlackControl.setPrecision(4);
   this.luptonBlackControl.setValue(this.luptonEngine.blackPoint);
   this.luptonBlackControl.toolTip = "Background level subtraction";
   this.luptonBlackControl.onValueUpdated = function(value) {
      dialog.luptonEngine.blackPoint = value;
      dialog.previewControl.luptonEngine.blackPoint = value;
      dialog.schedulePreviewUpdate();
   };

   this.luptonSatControl = new NumericControl(this);
   this.luptonSatControl.label.text = "Saturation:";
   this.luptonSatControl.label.setFixedWidth(80);
   this.luptonSatControl.setRange(0.5, 2.0);
   this.luptonSatControl.slider.setRange(0, 150);
   this.luptonSatControl.setPrecision(2);
   this.luptonSatControl.setValue(this.luptonEngine.saturation);
   this.luptonSatControl.toolTip = "Post-stretch saturation";
   this.luptonSatControl.onValueUpdated = function(value) {
      dialog.luptonEngine.saturation = value;
      dialog.previewControl.luptonEngine.saturation = value;
      dialog.schedulePreviewUpdate();
   };

   this.luptonClipLabel = new Label(this);
   this.luptonClipLabel.text = "Clipping:";
   this.luptonClipLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.luptonClipLabel.setFixedWidth(80);

   this.luptonClipCombo = new ComboBox(this);
   this.luptonClipCombo.addItem("Preserve Color (Lupton)");
   this.luptonClipCombo.addItem("Hard Clip");
   this.luptonClipCombo.addItem("Rescale to Max");
   this.luptonClipCombo.currentItem = 0;
   this.luptonClipCombo.toolTip = "How to handle values > 1.0";
   this.luptonClipCombo.onItemSelected = function(index) {
      dialog.luptonEngine.clippingMode = index;
      dialog.previewControl.luptonEngine.clippingMode = index;
      dialog.schedulePreviewUpdate();
   };

   var luptonClipSizer = new HorizontalSizer;
   luptonClipSizer.spacing = 4;
   luptonClipSizer.add(this.luptonClipLabel);
   luptonClipSizer.add(this.luptonClipCombo, 100);

   this.luptonPanel = new GroupBox(this);
   this.luptonPanel.title = "Lupton RGB Parameters";
   this.luptonPanel.sizer = new VerticalSizer;
   this.luptonPanel.sizer.margin = 6;
   this.luptonPanel.sizer.spacing = 4;
   this.luptonPanel.sizer.add(this.luptonStretchControl);
   this.luptonPanel.sizer.add(this.luptonQControl);
   this.luptonPanel.sizer.add(this.luptonBlackControl);
   this.luptonPanel.sizer.add(this.luptonSatControl);
   this.luptonPanel.sizer.add(luptonClipSizer);

   // =========================================================================
   // Root-power Parameters Panel
   // =========================================================================

   this.rpRootpowerControl = new NumericControl(this);
   this.rpRootpowerControl.label.text = "Root Power:";
   this.rpRootpowerControl.label.setFixedWidth(80);
   this.rpRootpowerControl.setRange(1.0, 200.0);
   this.rpRootpowerControl.slider.setRange(0, 1000);
   this.rpRootpowerControl.setPrecision(1);
   this.rpRootpowerControl.setValue(this.rpEngine.rootpower);
   this.rpRootpowerControl.toolTip = "Primary power stretch factor";
   this.rpRootpowerControl.onValueUpdated = function(value) {
      dialog.rpEngine.rootpower = value;
      dialog.previewControl.rpEngine.rootpower = value;
      dialog.schedulePreviewUpdate();
   };

   this.rpTwoPassCheck = new CheckBox(this);
   this.rpTwoPassCheck.text = "Two-Pass Stretch";
   this.rpTwoPassCheck.checked = this.rpEngine.twoPass;
   this.rpTwoPassCheck.onCheck = function(checked) {
      dialog.rpEngine.twoPass = checked;
      dialog.previewControl.rpEngine.twoPass = checked;
      dialog.rpRootpower2Control.enabled = checked;
      dialog.schedulePreviewUpdate();
   };

   this.rpRootpower2Control = new NumericControl(this);
   this.rpRootpower2Control.label.text = "Root Power 2:";
   this.rpRootpower2Control.label.setFixedWidth(80);
   this.rpRootpower2Control.setRange(1.0, 20.0);
   this.rpRootpower2Control.slider.setRange(0, 200);
   this.rpRootpower2Control.setPrecision(1);
   this.rpRootpower2Control.setValue(this.rpEngine.rootpower2);
   this.rpRootpower2Control.enabled = this.rpEngine.twoPass;
   this.rpRootpower2Control.toolTip = "Secondary power factor";
   this.rpRootpower2Control.onValueUpdated = function(value) {
      dialog.rpEngine.rootpower2 = value;
      dialog.previewControl.rpEngine.rootpower2 = value;
      dialog.schedulePreviewUpdate();
   };

   this.rpScurveLabel = new Label(this);
   this.rpScurveLabel.text = "S-Curve:";
   this.rpScurveLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.rpScurveLabel.setFixedWidth(80);

   this.rpScurveNone = new RadioButton(this);
   this.rpScurveNone.text = "None";
   this.rpScurveNone.checked = true;
   this.rpScurveNone.onCheck = function(checked) {
      if (checked) {
         dialog.rpEngine.scurve = 0;
         dialog.previewControl.rpEngine.scurve = 0;
         dialog.schedulePreviewUpdate();
      }
   };

   this.rpScurve1 = new RadioButton(this);
   this.rpScurve1.text = "SC1";
   this.rpScurve1.onCheck = function(checked) {
      if (checked) {
         dialog.rpEngine.scurve = 1;
         dialog.previewControl.rpEngine.scurve = 1;
         dialog.schedulePreviewUpdate();
      }
   };

   this.rpScurve2 = new RadioButton(this);
   this.rpScurve2.text = "SC2";
   this.rpScurve2.onCheck = function(checked) {
      if (checked) {
         dialog.rpEngine.scurve = 2;
         dialog.previewControl.rpEngine.scurve = 2;
         dialog.schedulePreviewUpdate();
      }
   };

   this.rpScurve3 = new RadioButton(this);
   this.rpScurve3.text = "SC3";
   this.rpScurve3.onCheck = function(checked) {
      if (checked) {
         dialog.rpEngine.scurve = 3;
         dialog.previewControl.rpEngine.scurve = 3;
         dialog.schedulePreviewUpdate();
      }
   };

   this.rpScurve4 = new RadioButton(this);
   this.rpScurve4.text = "SC4";
   this.rpScurve4.onCheck = function(checked) {
      if (checked) {
         dialog.rpEngine.scurve = 4;
         dialog.previewControl.rpEngine.scurve = 4;
         dialog.schedulePreviewUpdate();
      }
   };

   var rpScurveSizer = new HorizontalSizer;
   rpScurveSizer.spacing = 8;
   rpScurveSizer.add(this.rpScurveLabel);
   rpScurveSizer.add(this.rpScurveNone);
   rpScurveSizer.add(this.rpScurve1);
   rpScurveSizer.add(this.rpScurve2);
   rpScurveSizer.add(this.rpScurve3);
   rpScurveSizer.add(this.rpScurve4);
   rpScurveSizer.addStretch();

   this.rpEnhanceControl = new NumericControl(this);
   this.rpEnhanceControl.label.text = "Saturation:";
   this.rpEnhanceControl.label.setFixedWidth(80);
   this.rpEnhanceControl.setRange(0.5, 2.0);
   this.rpEnhanceControl.slider.setRange(0, 150);
   this.rpEnhanceControl.setPrecision(2);
   this.rpEnhanceControl.setValue(this.rpEngine.enhance);
   this.rpEnhanceControl.toolTip = "Post-stretch saturation enhancement";
   this.rpEnhanceControl.onValueUpdated = function(value) {
      dialog.rpEngine.enhance = value;
      dialog.previewControl.rpEngine.enhance = value;
      dialog.schedulePreviewUpdate();
   };

   this.rpPanel = new GroupBox(this);
   this.rpPanel.title = "Root-power Stretch Parameters";
   this.rpPanel.sizer = new VerticalSizer;
   this.rpPanel.sizer.margin = 6;
   this.rpPanel.sizer.spacing = 4;
   this.rpPanel.sizer.add(this.rpRootpowerControl);
   this.rpPanel.sizer.add(this.rpTwoPassCheck);
   this.rpPanel.sizer.add(this.rpRootpower2Control);
   this.rpPanel.sizer.add(rpScurveSizer);
   this.rpPanel.sizer.add(this.rpEnhanceControl);
   this.rpPanel.visible = false;

   // =========================================================================
   // Photometric Parameters Panel
   // =========================================================================

   this.photoQControl = new NumericControl(this);
   this.photoQControl.label.text = "Stretch (\u03B1):";
   this.photoQControl.label.setFixedWidth(80);
   this.photoQControl.setRange(0.1, 1000.0);
   this.photoQControl.slider.setRange(0, 10000);
   this.photoQControl.setPrecision(1);
   this.photoQControl.setValue(this.photoEngine.Q);
   this.photoQControl.toolTip = "Linear amplification factor (log scale)";
   this.photoQControl.onValueUpdated = function(value) {
      dialog.photoEngine.Q = value;
      dialog.previewControl.photoEngine.Q = value;
      dialog.schedulePreviewUpdate();
   };

   this.photoSoftControl = new NumericControl(this);
   this.photoSoftControl.label.text = "Q (softening):";
   this.photoSoftControl.label.setFixedWidth(80);
   this.photoSoftControl.setRange(0.0, 100.0);
   this.photoSoftControl.slider.setRange(0, 1000);
   this.photoSoftControl.setPrecision(1);
   this.photoSoftControl.setValue(this.photoEngine.softening);
   this.photoSoftControl.toolTip = "Controls linear/log transition";
   this.photoSoftControl.onValueUpdated = function(value) {
      dialog.photoEngine.softening = value;
      dialog.previewControl.photoEngine.softening = value;
      dialog.schedulePreviewUpdate();
   };

   this.photoBlackControl = new NumericControl(this);
   this.photoBlackControl.label.text = "Black Point:";
   this.photoBlackControl.label.setFixedWidth(80);
   this.photoBlackControl.setRange(0, 0.01);
   this.photoBlackControl.slider.setRange(0, 1000);
   this.photoBlackControl.setPrecision(4);
   this.photoBlackControl.setValue(this.photoEngine.blackPoint);
   this.photoBlackControl.toolTip = "Background subtraction level";
   this.photoBlackControl.onValueUpdated = function(value) {
      dialog.photoEngine.blackPoint = value;
      dialog.previewControl.photoEngine.blackPoint = value;
      dialog.schedulePreviewUpdate();
   };

   this.photoSatControl = new NumericControl(this);
   this.photoSatControl.label.text = "Saturation:";
   this.photoSatControl.label.setFixedWidth(80);
   this.photoSatControl.setRange(0.5, 2.0);
   this.photoSatControl.slider.setRange(0, 150);
   this.photoSatControl.setPrecision(2);
   this.photoSatControl.setValue(this.photoEngine.saturation);
   this.photoSatControl.toolTip = "Post-stretch saturation";
   this.photoSatControl.onValueUpdated = function(value) {
      dialog.photoEngine.saturation = value;
      dialog.previewControl.photoEngine.saturation = value;
      dialog.schedulePreviewUpdate();
   };

   this.photoPanel = new GroupBox(this);
   this.photoPanel.title = "PhotometricStretch Parameters";
   this.photoPanel.sizer = new VerticalSizer;
   this.photoPanel.sizer.margin = 6;
   this.photoPanel.sizer.spacing = 4;
   this.photoPanel.sizer.add(this.photoQControl);
   this.photoPanel.sizer.add(this.photoSoftControl);
   this.photoPanel.sizer.add(this.photoBlackControl);
   this.photoPanel.sizer.add(this.photoSatControl);
   this.photoPanel.visible = false;

   // =========================================================================
   // Preview Mode Controls
   // =========================================================================

   this.beforeButton = new PushButton(this);
   this.beforeButton.text = "Before";
   this.beforeButton.setFixedWidth(55);
   this.beforeButton.onClick = function() {
      dialog.previewControl.previewMode = 1;
      dialog.updatePreviewButtons();
      dialog.schedulePreviewUpdate();
   };

   this.splitButton = new PushButton(this);
   this.splitButton.text = "[Split]";
   this.splitButton.setFixedWidth(55);
   this.splitButton.onClick = function() {
      dialog.previewControl.previewMode = 2;
      dialog.updatePreviewButtons();
      dialog.schedulePreviewUpdate();
   };

   this.afterButton = new PushButton(this);
   this.afterButton.text = "After";
   this.afterButton.setFixedWidth(55);
   this.afterButton.onClick = function() {
      dialog.previewControl.previewMode = 0;
      dialog.updatePreviewButtons();
      dialog.schedulePreviewUpdate();
   };

   // =========================================================================
   // Zoom Controls: [zoom-out] [Fit] [zoom-in]
   // =========================================================================

   this.zoomOutButton = new ToolButton(this);
   this.zoomOutButton.icon = this.scaledResource(":/icons/zoom-out.png");
   this.zoomOutButton.setScaledFixedSize(24, 24);
   this.zoomOutButton.toolTip = "Zoom out (or use mouse wheel)";
   this.zoomOutButton.onClick = function() {
      dialog.previewControl.zoomOut();
   };

   this.zoomFitButton = new PushButton(this);
   this.zoomFitButton.text = "Fit";
   this.zoomFitButton.setFixedWidth(40);
   this.zoomFitButton.toolTip = "Fit to window";
   this.zoomFitButton.onClick = function() {
      dialog.previewControl.zoomToFit();
   };

   this.zoomInButton = new ToolButton(this);
   this.zoomInButton.icon = this.scaledResource(":/icons/zoom-in.png");
   this.zoomInButton.setScaledFixedSize(24, 24);
   this.zoomInButton.toolTip = "Zoom in (or use mouse wheel)";
   this.zoomInButton.onClick = function() {
      dialog.previewControl.zoomIn();
   };

   // Preview mode + zoom on same line: [Before][Split][After] ... [-][Fit][+]
   var previewControlsSizer = new HorizontalSizer;
   previewControlsSizer.spacing = 6;
   previewControlsSizer.add(this.beforeButton);
   previewControlsSizer.add(this.splitButton);
   previewControlsSizer.add(this.afterButton);
   previewControlsSizer.addStretch();
   previewControlsSizer.add(this.zoomOutButton);
   previewControlsSizer.addSpacing(2);
   previewControlsSizer.add(this.zoomFitButton);
   previewControlsSizer.addSpacing(2);
   previewControlsSizer.add(this.zoomInButton);

   // =========================================================================
   // Preview Control
   // =========================================================================

   this.previewControl = new UnifiedPreviewControl(this);
   this.previewControl.setMinSize(500, 400);
   this.previewControl.previewMode = 2; // Split by default

   this.previewGroup = new GroupBox(this);
   this.previewGroup.title = "Preview";
   this.previewGroup.sizer = new VerticalSizer;
   this.previewGroup.sizer.margin = 6;
   this.previewGroup.sizer.spacing = 6;
   this.previewGroup.sizer.add(previewControlsSizer);
   this.previewGroup.sizer.add(this.previewControl, 100);

   // =========================================================================
   // Buttons
   // =========================================================================

   this.resetButton = new PushButton(this);
   this.resetButton.text = "Reset";
   this.resetButton.toolTip = "Reset parameters to defaults";
   this.resetButton.onClick = function() {
      dialog.resetCurrentEngine();
   };

   this.executeButton = new PushButton(this);
   this.executeButton.text = "Execute";
   this.executeButton.toolTip = "Apply stretch to create new image";
   this.executeButton.onClick = function() {
      dialog.executeStretch();
   };

   this.closeButton = new PushButton(this);
   this.closeButton.text = "Close";
   this.closeButton.onClick = function() {
      dialog.cancel();
   };

   var buttonSizer = new HorizontalSizer;
   buttonSizer.spacing = 6;
   buttonSizer.addStretch();
   buttonSizer.add(this.resetButton);
   buttonSizer.add(this.executeButton);
   buttonSizer.add(this.closeButton);

   // =========================================================================
   // Layout
   // =========================================================================

   var leftPanel = new Control(this);
   leftPanel.setMinWidth(300);
   leftPanel.setMaxWidth(450);
   leftPanel.sizer = new VerticalSizer;
   leftPanel.sizer.margin = 6;
   leftPanel.sizer.spacing = 8;
   leftPanel.sizer.add(this.inputGroup);
   leftPanel.sizer.add(this.luptonPanel);
   leftPanel.sizer.add(this.rpPanel);
   leftPanel.sizer.add(this.photoPanel);
   leftPanel.sizer.addStretch();
   leftPanel.sizer.add(buttonSizer);

   var mainSizer = new HorizontalSizer;
   mainSizer.spacing = 8;
   mainSizer.add(leftPanel);
   mainSizer.add(this.previewGroup, 100);

   this.sizer = new VerticalSizer;
   this.sizer.margin = 8;
   this.sizer.add(mainSizer, 100);

   // Set initial dialog size (larger for better preview)
   this.resize(1600, 1000);

   // =========================================================================
   // Helper Methods
   // =========================================================================

   this.updateParameterPanels = function() {
      this.luptonPanel.visible = (this.algorithm === Algorithm.LUPTON);
      this.rpPanel.visible = (this.algorithm === Algorithm.ROOTPOWER);
      this.photoPanel.visible = (this.algorithm === Algorithm.PHOTOMETRIC);
      // Don't call adjustToContents() - keep dialog size stable when switching algorithms
   };

   this.updatePreviewButtons = function() {
      var mode = this.previewControl.previewMode;
      this.beforeButton.text = (mode === 1) ? "[Before]" : "Before";
      this.splitButton.text = (mode === 2) ? "[Split]" : "Split";
      this.afterButton.text = (mode === 0) ? "[After]" : "After";
   };

   this.updateZoomButton = function() {
      if (this.previewControl.zoomFit) {
         this.zoomFitButton.text = "Fit";
      } else {
         this.zoomFitButton.text = this.previewControl.getZoomPercent() + "%";
      }
   };

   this.schedulePreviewUpdate = function() {
      var now = new Date().getTime();
      var elapsed = now - this.lastPreviewTime;

      if (elapsed < 80) {
         this.previewSkipCount++;
         if (this.previewSkipCount < 4) return;
      }

      this.previewSkipCount = 0;
      this.lastPreviewTime = now;
      this.previewControl.updatePreview();
   };

   this.resetCurrentEngine = function() {
      switch (this.algorithm) {
         case Algorithm.LUPTON:
            this.luptonEngine.reset();
            this.luptonStretchControl.setValue(this.luptonEngine.stretch);
            this.luptonQControl.setValue(this.luptonEngine.Q);
            this.luptonBlackControl.setValue(this.luptonEngine.blackPoint);
            this.luptonSatControl.setValue(this.luptonEngine.saturation);
            this.luptonClipCombo.currentItem = this.luptonEngine.clippingMode;
            this.previewControl.luptonEngine = this.luptonEngine;
            break;
         case Algorithm.ROOTPOWER:
            this.rpEngine.reset();
            this.rpRootpowerControl.setValue(this.rpEngine.rootpower);
            this.rpRootpower2Control.setValue(this.rpEngine.rootpower2);
            this.rpTwoPassCheck.checked = this.rpEngine.twoPass;
            this.rpRootpower2Control.enabled = this.rpEngine.twoPass;
            this.rpScurveNone.checked = true;
            this.rpEnhanceControl.setValue(this.rpEngine.enhance);
            this.previewControl.rpEngine = this.rpEngine;
            break;
         case Algorithm.PHOTOMETRIC:
            this.photoEngine.reset();
            this.photoQControl.setValue(this.photoEngine.Q);
            this.photoSoftControl.setValue(this.photoEngine.softening);
            this.photoBlackControl.setValue(this.photoEngine.blackPoint);
            this.photoSatControl.setValue(this.photoEngine.saturation);
            this.previewControl.photoEngine = this.photoEngine;
            break;
      }
      this.schedulePreviewUpdate();
   };

   this.executeStretch = function() {
      if (!this.targetWindow) {
         (new MessageBox("Please select a target image.", TITLE, StdIcon_Error, StdButton_Ok)).execute();
         return;
      }

      console.show();
      console.writeln("<b>" + TITLE + " v" + VERSION + "</b>");
      console.writeln("Algorithm: " + AlgorithmNames[this.algorithm]);
      console.writeln("Processing: " + this.targetWindow.mainView.id);

      var startTime = new Date().getTime();
      var result = null;

      switch (this.algorithm) {
         case Algorithm.LUPTON:
            result = this.luptonEngine.execute(this.targetWindow);
            break;
         case Algorithm.ROOTPOWER:
            result = this.rpEngine.execute(this.targetWindow);
            break;
         case Algorithm.PHOTOMETRIC:
            result = this.photoEngine.execute(this.targetWindow);
            break;
      }

      var elapsed = (new Date().getTime() - startTime) / 1000;

      if (result) {
         console.writeln(format("Completed in %.2f seconds", elapsed));
      } else {
         console.criticalln("Processing failed");
      }
   };

   // Initialize
   this.populateImageList();
   if (this.targetWindow) {
      this.previewControl.sourceWindow = this.targetWindow;
   }

   // Setup zoom change callback
   var self = this;
   this.previewControl.onZoomChanged = function() {
      self.updateZoomButton();
   };

   this.updatePreviewButtons();
   this.updateZoomButton();
   this.schedulePreviewUpdate();
}

EZStretchDialog.prototype = new Dialog;

// ============================================================================
// Main Entry Point
// ============================================================================

function main() {
   console.hide();

   if (ImageWindow.activeWindow.isNull) {
      (new MessageBox(
         "Please open an RGB image before running this script.",
         TITLE,
         StdIcon_Error,
         StdButton_Ok
      )).execute();
      return;
   }

   var dialog = new EZStretchDialog();
   dialog.execute();
}

main();
