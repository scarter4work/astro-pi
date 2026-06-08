// ============================================================================
// RNC-ColorStretch.js
// PixInsight JavaScript script for RNC Color Stretch
//
// Based on Roger N. Clark's rnc-color-stretch algorithm
// https://clarkvision.com/articles/astrophotography-rnc-color-stretch/
//
// Copyright (c) 2025. All rights reserved.
// Released under GPL License (same as original algorithm)
// ============================================================================

#feature-id    EZ Stretch BSC > RNC-ColorStretch
#script-id     RNC-ColorStretch
#feature-info  <b>RNC-ColorStretch</b> is an advanced image stretching algorithm \
   designed specifically for astrophotography. Unlike traditional stretching methods \
   that cause color loss (especially in faint nebulae and star fields), RNC-ColorStretch \
   actively <b>recovers</b> color that would otherwise be lost during the stretch process.<br/>\
   <br/>\
   <b>Important:</b> This algorithm is designed for LINEAR (unstretched) images. \
   Using it on already-stretched data will produce over-exposed results.<br/>\
   <br/>\
   Based on Roger N. Clark's rnc-color-stretch algorithm.<br/>\
   <br/>\
   Copyright &copy; 2025. Released under GPL License.

#define VERSION "1.0.11"
#define TITLE "RNC-ColorStretch"

"use strict";

#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/TextAlign.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/SectionBar.jsh>
#include <pjsr/Color.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/UndoFlag.jsh>
#include <pjsr/ImageOp.jsh>

// Version check - require PixInsight 1.8.0 or higher
#iflt __PI_VERSION__ 01.08.00
#error This script requires PixInsight 1.8.0 or higher.
#endif

// Enable automatic garbage collection
var jsAutoGC = true;

// ============================================================================
// RNCColorStretchParameters - Parameter container and persistence
// ============================================================================
function RNCColorStretchParameters() {
   // Stretch parameters
   this.rootpower = 6.0;           // Primary power stretch factor (1.0 - 599.0)
   this.rootpower2 = 3.0;          // Secondary power factor (1.0 - 599.0)
   this.twoPass = false;           // Enable two-pass stretch
   this.scurve = 0;                // S-curve selection (0=none, 1-4=variants)

   // Sky calibration
   this.skylevelfactor = 0.06;     // Sky level as fraction of histogram peak
   this.linkSkyZero = true;        // Link RGB sky zero values
   this.rgbskyzero = [4096, 4096, 4096];  // Target sky zero point per channel (16-bit)

   // Color recovery
   this.colorRecovery = true;      // Enable color recovery
   this.colorLimit = 0.2;          // Lower limit for color ratio clamping
   this.enhance = 1.0;             // Post-stretch saturation multiplier

   // Advanced options
   this.setmin = [5140, 5200, 5650];  // Minimum RGB levels (noise floor, 16-bit)
   this.clipPercent = 0.00005;     // Max fraction of pixels clipped to zero
   this.toneCurve = false;         // Apply tone curve for linear input

   // Processing options
   this.createNewImage = true;     // Create new image vs modify in place
}

RNCColorStretchParameters.prototype.LIMITS = {
   rootpower: { min: 1.0, max: 599.0, default: 6.0 },
   rootpower2: { min: 1.0, max: 599.0, default: 3.0 },
   skylevelfactor: { min: 0.0001, max: 0.5, default: 0.06 },
   rgbskyzero: { min: 0, max: 65535, default: 4096 },
   setmin: { min: 0, max: 65535, default: 5140 },
   colorLimit: { min: 0.1, max: 1.0, default: 0.2 },
   enhance: { min: 0.1, max: 3.0, default: 1.0 },
   clipPercent: { min: 0, max: 1.0, default: 0.00005 }
};

RNCColorStretchParameters.prototype.reset = function() {
   this.rootpower = 6.0;
   this.rootpower2 = 3.0;
   this.twoPass = false;
   this.scurve = 0;
   this.skylevelfactor = 0.06;
   this.linkSkyZero = true;
   this.rgbskyzero = [4096, 4096, 4096];
   this.colorRecovery = true;
   this.colorLimit = 0.2;
   this.enhance = 1.0;
   this.setmin = [5140, 5200, 5650];
   this.clipPercent = 0.00005;
   this.toneCurve = false;
   this.createNewImage = true;
};

RNCColorStretchParameters.prototype.load = function() {
   // Settings persistence not used - parameters come from process icons
};

RNCColorStretchParameters.prototype.save = function() {
   // Settings persistence not used - parameters saved via process icons
};

RNCColorStretchParameters.prototype.exportParameters = function() {
   Parameters.set("rootpower", this.rootpower);
   Parameters.set("rootpower2", this.rootpower2);
   Parameters.set("twoPass", this.twoPass);
   Parameters.set("scurve", this.scurve);
   Parameters.set("skylevelfactor", this.skylevelfactor);
   Parameters.set("linkSkyZero", this.linkSkyZero);
   Parameters.set("rgbskyzeroR", this.rgbskyzero[0]);
   Parameters.set("rgbskyzeroG", this.rgbskyzero[1]);
   Parameters.set("rgbskyzeroB", this.rgbskyzero[2]);
   Parameters.set("colorRecovery", this.colorRecovery);
   Parameters.set("colorLimit", this.colorLimit);
   Parameters.set("enhance", this.enhance);
   Parameters.set("setminR", this.setmin[0]);
   Parameters.set("setminG", this.setmin[1]);
   Parameters.set("setminB", this.setmin[2]);
   Parameters.set("clipPercent", this.clipPercent);
   Parameters.set("toneCurve", this.toneCurve);
   Parameters.set("createNewImage", this.createNewImage);
};

RNCColorStretchParameters.prototype.importParameters = function() {
   if (Parameters.has("rootpower"))
      this.rootpower = Parameters.getReal("rootpower");
   if (Parameters.has("rootpower2"))
      this.rootpower2 = Parameters.getReal("rootpower2");
   if (Parameters.has("twoPass"))
      this.twoPass = Parameters.getBoolean("twoPass");
   if (Parameters.has("scurve"))
      this.scurve = Parameters.getInteger("scurve");
   if (Parameters.has("skylevelfactor"))
      this.skylevelfactor = Parameters.getReal("skylevelfactor");
   if (Parameters.has("linkSkyZero"))
      this.linkSkyZero = Parameters.getBoolean("linkSkyZero");
   if (Parameters.has("rgbskyzeroR"))
      this.rgbskyzero[0] = Parameters.getInteger("rgbskyzeroR");
   if (Parameters.has("rgbskyzeroG"))
      this.rgbskyzero[1] = Parameters.getInteger("rgbskyzeroG");
   if (Parameters.has("rgbskyzeroB"))
      this.rgbskyzero[2] = Parameters.getInteger("rgbskyzeroB");
   if (Parameters.has("colorRecovery"))
      this.colorRecovery = Parameters.getBoolean("colorRecovery");
   if (Parameters.has("colorLimit"))
      this.colorLimit = Parameters.getReal("colorLimit");
   if (Parameters.has("enhance"))
      this.enhance = Parameters.getReal("enhance");
   if (Parameters.has("setminR"))
      this.setmin[0] = Parameters.getInteger("setminR");
   if (Parameters.has("setminG"))
      this.setmin[1] = Parameters.getInteger("setminG");
   if (Parameters.has("setminB"))
      this.setmin[2] = Parameters.getInteger("setminB");
   if (Parameters.has("clipPercent"))
      this.clipPercent = Parameters.getReal("clipPercent");
   if (Parameters.has("toneCurve"))
      this.toneCurve = Parameters.getBoolean("toneCurve");
   if (Parameters.has("createNewImage"))
      this.createNewImage = Parameters.getBoolean("createNewImage");
};

RNCColorStretchParameters.prototype.validate = function() {
   var L = this.LIMITS;
   this.rootpower = Math.max(L.rootpower.min, Math.min(L.rootpower.max, this.rootpower));
   this.rootpower2 = Math.max(L.rootpower2.min, Math.min(L.rootpower2.max, this.rootpower2));
   this.scurve = Math.max(0, Math.min(4, Math.round(this.scurve)));
   this.skylevelfactor = Math.max(L.skylevelfactor.min, Math.min(L.skylevelfactor.max, this.skylevelfactor));
   for (var i = 0; i < 3; i++) {
      this.rgbskyzero[i] = Math.max(L.rgbskyzero.min, Math.min(L.rgbskyzero.max, Math.round(this.rgbskyzero[i])));
      this.setmin[i] = Math.max(L.setmin.min, Math.min(L.setmin.max, Math.round(this.setmin[i])));
   }
   this.colorLimit = Math.max(L.colorLimit.min, Math.min(L.colorLimit.max, this.colorLimit));
   this.enhance = Math.max(L.enhance.min, Math.min(L.enhance.max, this.enhance));
   this.clipPercent = Math.max(L.clipPercent.min, Math.min(L.clipPercent.max, this.clipPercent));
};

// ============================================================================
// RNCColorStretchEngine - Core processing engine
// ============================================================================
function RNCColorStretchEngine() {
   this.parameters = null;
   this.abortRequested = false;
}

RNCColorStretchEngine.prototype.apply = function(image, isPreview) {
   if (!this.parameters) {
      throw new Error("Parameters not set");
   }
   if (image.numberOfChannels < 3) {
      throw new Error("RNC-ColorStretch requires an RGB color image");
   }

   isPreview = isPreview || false;
   var P = this.parameters;

   // Calculate total steps for progress
   var totalSteps = 6; // base steps
   if (P.twoPass && P.rootpower2 > 1) totalSteps += 1;
   if (P.scurve > 0) totalSteps += 1;
   if (P.colorRecovery) totalSteps += 1;
   if (P.enhance != 1.0) totalSteps += 1;
   var currentStep = 0;

   var logProgress = function(msg) {
      if (!isPreview) {
         currentStep++;
         var pct = Math.round((currentStep / totalSteps) * 100);
         console.writeln("<b>RNC-ColorStretch [" + pct + "%]:</b> " + msg);
         console.flush();
      }
   };

   // Store original color ratios before any processing (for color recovery)
   var originalRatios = null;
   if (P.colorRecovery) {
      logProgress("Computing original color ratios...");
      originalRatios = this.computeColorRatios(image);
   }

   // Step 1: Analyze histogram and determine sky levels
   logProgress("Analyzing sky levels...");
   var skyLevels = this.analyzeSkyLevels(image);

   // Step 2: Subtract sky offset per channel
   logProgress("Subtracting sky background...");
   this.subtractSky(image, skyLevels);

   // Step 3: Apply power stretch (pass 1)
   logProgress("Applying power stretch (pass 1, rootpower=" + P.rootpower.toFixed(1) + ")...");
   this.applyPowerStretch(image, P.rootpower);

   // Re-analyze and re-align sky
   skyLevels = this.analyzeSkyLevels(image);
   this.subtractSky(image, skyLevels);

   // Step 4: Apply power stretch (pass 2, if enabled)
   if (P.twoPass && P.rootpower2 > 1) {
      logProgress("Applying power stretch (pass 2, rootpower2=" + P.rootpower2.toFixed(1) + ")...");
      this.applyPowerStretch(image, P.rootpower2);
      skyLevels = this.analyzeSkyLevels(image);
      this.subtractSky(image, skyLevels);
   }

   // Step 5: Apply S-curve if selected
   if (P.scurve > 0) {
      logProgress("Applying S-curve " + P.scurve + "...");
      this.applySCurve(image, P.scurve);
   }

   // Step 6: Color recovery
   if (P.colorRecovery && originalRatios) {
      logProgress("Recovering color...");
      var stretchedRatios = this.computeColorRatios(image);
      this.recoverColor(image, originalRatios, stretchedRatios);
   }

   // Step 7: Apply enhancement (saturation)
   if (P.enhance != 1.0) {
      logProgress("Applying saturation enhancement (" + P.enhance.toFixed(2) + ")...");
      this.applySaturation(image, P.enhance);
   }

   // Step 8: Apply minimum levels
   logProgress("Applying minimum levels...");
   this.applyMinimumLevels(image);

   // Ensure values are in valid range [0,1]
   image.truncate(0, 1);

   if (!isPreview) {
      console.writeln("<b>RNC-ColorStretch [100%]:</b> Processing complete.");
      console.flush();
   }
};

RNCColorStretchEngine.prototype.analyzeSkyLevels = function(image) {
   var P = this.parameters;
   var skyLevels = [];
   var histogramResolution = 65536;
   var rect = new Rect(0, 0, image.width, image.height);

   for (var c = 0; c < 3; c++) {
      // Get pixel samples for this channel
      var samples = [];
      image.getSamples(samples, rect, c);

      // Build histogram manually
      var histogram = new Array(histogramResolution);
      for (var i = 0; i < histogramResolution; i++) {
         histogram[i] = 0;
      }

      for (var i = 0; i < samples.length; i++) {
         var bin = Math.floor(samples[i] * (histogramResolution - 1));
         bin = Math.max(0, Math.min(histogramResolution - 1, bin));
         histogram[bin]++;
      }

      // Find histogram peak (mode)
      var peakBin = 0;
      var peakCount = 0;

      for (var i = 1; i < histogram.length; i++) {
         if (histogram[i] > peakCount) {
            peakCount = histogram[i];
            peakBin = i;
         }
      }

      // Find sky level at skylevelfactor of peak count
      var targetCount = peakCount * P.skylevelfactor;
      var skyBin = peakBin;

      while (skyBin > 0 && histogram[skyBin] > targetCount) {
         skyBin--;
      }

      var skyLevel = skyBin / (histogramResolution - 1);
      skyLevels.push(skyLevel);
   }

   return skyLevels;
};

RNCColorStretchEngine.prototype.subtractSky = function(image, skyLevels) {
   var P = this.parameters;
   var targetSky = [
      P.rgbskyzero[0] / 65535.0,
      P.rgbskyzero[1] / 65535.0,
      P.rgbskyzero[2] / 65535.0
   ];

   for (var c = 0; c < 3; c++) {
      var offset = skyLevels[c] - targetSky[c];
      if (Math.abs(offset) > 0.0001) {
         image.apply(offset, ImageOp_Sub, new Rect, c, c);
      }
   }
};

RNCColorStretchEngine.prototype.applyPowerStretch = function(image, rootpower) {
   if (rootpower <= 1) return;
   var exponent = 1.0 / rootpower;
   image.apply(exponent, ImageOp_Pow);
};

RNCColorStretchEngine.prototype.computeColorRatios = function(image) {
   var P = this.parameters;
   var width = image.width;
   var height = image.height;
   var rect = new Rect(0, 0, width, height);

   var R = [];
   var G = [];
   var B = [];

   image.getSamples(R, rect, 0);
   image.getSamples(G, rect, 1);
   image.getSamples(B, rect, 2);

   var numPixels = R.length;
   var colorLimit = P.colorLimit;
   var colorLimitHigh = 1.0 / colorLimit;
   var threshold = 0.001;

   var ratios = {
      gr: new Float32Array(numPixels),
      br: new Float32Array(numPixels),
      rg: new Float32Array(numPixels),
      bg: new Float32Array(numPixels),
      gb: new Float32Array(numPixels),
      rb: new Float32Array(numPixels)
   };

   for (var i = 0; i < numPixels; i++) {
      var r = R[i];
      var g = G[i];
      var b = B[i];

      if (r > threshold) {
         ratios.gr[i] = Math.max(colorLimit, Math.min(colorLimitHigh, g / r));
         ratios.br[i] = Math.max(colorLimit, Math.min(colorLimitHigh, b / r));
      } else {
         ratios.gr[i] = 1.0;
         ratios.br[i] = 1.0;
      }

      if (g > threshold) {
         ratios.rg[i] = Math.max(colorLimit, Math.min(colorLimitHigh, r / g));
         ratios.bg[i] = Math.max(colorLimit, Math.min(colorLimitHigh, b / g));
      } else {
         ratios.rg[i] = 1.0;
         ratios.bg[i] = 1.0;
      }

      if (b > threshold) {
         ratios.gb[i] = Math.max(colorLimit, Math.min(colorLimitHigh, g / b));
         ratios.rb[i] = Math.max(colorLimit, Math.min(colorLimitHigh, r / b));
      } else {
         ratios.gb[i] = 1.0;
         ratios.rb[i] = 1.0;
      }
   }

   return ratios;
};

RNCColorStretchEngine.prototype.recoverColor = function(image, originalRatios, stretchedRatios) {
   var P = this.parameters;
   var width = image.width;
   var height = image.height;
   var rect = new Rect(0, 0, width, height);

   var R = [];
   var G = [];
   var B = [];

   image.getSamples(R, rect, 0);
   image.getSamples(G, rect, 1);
   image.getSamples(B, rect, 2);

   var numPixels = R.length;

   for (var i = 0; i < numPixels; i++) {
      var r = R[i];
      var g = G[i];
      var b = B[i];

      var intensity = (r + g + b) / 3.0;
      var blend = Math.min(1.0, intensity * 3.0);
      blend = blend * blend;

      var cfGR = 1.0, cfBR = 1.0, cfRG = 1.0, cfBG = 1.0, cfGB = 1.0, cfRB = 1.0;

      if (stretchedRatios.gr[i] > 0.001) {
         cfGR = originalRatios.gr[i] / stretchedRatios.gr[i];
      }
      if (stretchedRatios.br[i] > 0.001) {
         cfBR = originalRatios.br[i] / stretchedRatios.br[i];
      }
      if (stretchedRatios.rg[i] > 0.001) {
         cfRG = originalRatios.rg[i] / stretchedRatios.rg[i];
      }
      if (stretchedRatios.bg[i] > 0.001) {
         cfBG = originalRatios.bg[i] / stretchedRatios.bg[i];
      }
      if (stretchedRatios.gb[i] > 0.001) {
         cfGB = originalRatios.gb[i] / stretchedRatios.gb[i];
      }
      if (stretchedRatios.rb[i] > 0.001) {
         cfRB = originalRatios.rb[i] / stretchedRatios.rb[i];
      }

      var rCorr = Math.sqrt(cfRG * cfRB);
      var gCorr = Math.sqrt(cfGR * cfGB);
      var bCorr = Math.sqrt(cfBR * cfBG);

      R[i] = r * (1.0 - blend + blend * rCorr);
      G[i] = g * (1.0 - blend + blend * gCorr);
      B[i] = b * (1.0 - blend + blend * bCorr);
   }

   image.setSamples(R, rect, 0);
   image.setSamples(G, rect, 1);
   image.setSamples(B, rect, 2);
};

RNCColorStretchEngine.prototype.applySCurve = function(image, curveType) {
   var width = image.width;
   var height = image.height;
   var rect = new Rect(0, 0, width, height);

   for (var c = 0; c < 3; c++) {
      var samples = [];
      image.getSamples(samples, rect, c);

      for (var i = 0; i < samples.length; i++) {
         var x = samples[i];
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

         samples[i] = Math.max(0, Math.min(1, y));
      }

      image.setSamples(samples, rect, c);
   }
};

RNCColorStretchEngine.prototype.applySaturation = function(image, factor) {
   var width = image.width;
   var height = image.height;
   var rect = new Rect(0, 0, width, height);

   var R = [];
   var G = [];
   var B = [];

   image.getSamples(R, rect, 0);
   image.getSamples(G, rect, 1);
   image.getSamples(B, rect, 2);

   var numPixels = R.length;

   for (var i = 0; i < numPixels; i++) {
      var r = R[i];
      var g = G[i];
      var b = B[i];
      var lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;

      R[i] = lum + (r - lum) * factor;
      G[i] = lum + (g - lum) * factor;
      B[i] = lum + (b - lum) * factor;
   }

   image.setSamples(R, rect, 0);
   image.setSamples(G, rect, 1);
   image.setSamples(B, rect, 2);
};

RNCColorStretchEngine.prototype.applyMinimumLevels = function(image) {
   var P = this.parameters;
   var minR = P.setmin[0] / 65535.0;
   var minG = P.setmin[1] / 65535.0;
   var minB = P.setmin[2] / 65535.0;

   var width = image.width;
   var height = image.height;
   var rect = new Rect(0, 0, width, height);

   var R = [];
   var G = [];
   var B = [];

   image.getSamples(R, rect, 0);
   image.getSamples(G, rect, 1);
   image.getSamples(B, rect, 2);

   for (var i = 0; i < R.length; i++) {
      R[i] = Math.max(R[i], minR);
      G[i] = Math.max(G[i], minG);
      B[i] = Math.max(B[i], minB);
   }

   image.setSamples(R, rect, 0);
   image.setSamples(G, rect, 1);
   image.setSamples(B, rect, 2);
};

RNCColorStretchEngine.prototype.autoDetectSky = function(image) {
   var skyLevels = this.analyzeSkyLevels(image);
   return [
      Math.round(skyLevels[0] * 65535),
      Math.round(skyLevels[1] * 65535),
      Math.round(skyLevels[2] * 65535)
   ];
};

RNCColorStretchEngine.prototype.log = function(message) {
   console.writeln("<b>RNC-ColorStretch:</b> " + message);
};

// Process a single pixel for preview (simplified version without color recovery)
RNCColorStretchEngine.prototype.processPixel = function(r, g, b) {
   var P = this.parameters;

   // Apply power stretch
   var exponent = 1.0 / P.rootpower;
   r = Math.pow(Math.max(0, r), exponent);
   g = Math.pow(Math.max(0, g), exponent);
   b = Math.pow(Math.max(0, b), exponent);

   // Second pass if enabled
   if (P.twoPass && P.rootpower2 > 1) {
      exponent = 1.0 / P.rootpower2;
      r = Math.pow(r, exponent);
      g = Math.pow(g, exponent);
      b = Math.pow(b, exponent);
   }

   // Apply S-curve
   if (P.scurve > 0) {
      r = this.applySCurveValue(r, P.scurve);
      g = this.applySCurveValue(g, P.scurve);
      b = this.applySCurveValue(b, P.scurve);
   }

   // Apply saturation
   if (P.enhance != 1.0) {
      var lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
      r = lum + (r - lum) * P.enhance;
      g = lum + (g - lum) * P.enhance;
      b = lum + (b - lum) * P.enhance;
   }

   return [
      Math.max(0, Math.min(1, r)),
      Math.max(0, Math.min(1, g)),
      Math.max(0, Math.min(1, b))
   ];
};

RNCColorStretchEngine.prototype.applySCurveValue = function(x, curveType) {
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

// Generate preview bitmap at specified size
RNCColorStretchEngine.prototype.generatePreviewAtSize = function(sourceWindow, outWidth, outHeight, previewMode, splitPos) {
   if (!sourceWindow) return null;
   if (outWidth <= 0 || outHeight <= 0) return null;

   var image = sourceWindow.mainView.image;
   if (!image || image.numberOfChannels < 3) return null;

   var imgWidth = image.width;
   var imgHeight = image.height;

   var bitmap = new Bitmap(outWidth, outHeight);

   var scaleX = imgWidth / outWidth;
   var scaleY = imgHeight / outHeight;

   var splitX = outWidth * splitPos / 100;

   for (var py = 0; py < outHeight; py++) {
      var iy = Math.min(Math.floor(py * scaleY), imgHeight - 1);

      for (var px = 0; px < outWidth; px++) {
         var ix = Math.min(Math.floor(px * scaleX), imgWidth - 1);

         var r = image.sample(ix, iy, 0);
         var g = image.sample(ix, iy, 1);
         var b = image.sample(ix, iy, 2);

         var rOut, gOut, bOut;

         // previewMode: 0=After, 1=Before, 2=Split
         var isBefore = (previewMode === 1) || (previewMode === 2 && px < splitX);

         if (isBefore) {
            // Show original with basic stretch for visibility
            rOut = Math.min(1, r * 10);
            gOut = Math.min(1, g * 10);
            bOut = Math.min(1, b * 10);
         } else {
            var result = this.processPixel(r, g, b);
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

// ============================================================================
// PreviewControl - Based on PJSR pattern (Frame + ScrollBox + VectorGraphics)
// ============================================================================
function PreviewControl(parent, engine) {
   this.__base__ = Frame;
   this.__base__(parent);

   this.engine = engine;
   this.scaledImage = null;
   this.sourceWindow = null;
   this.previewMode = 0;  // 0=After, 1=Before, 2=Split
   this.splitPosition = 50;
   this.zoomFactor = 1.0;  // 1.0 = fit to viewport
   this.zoomLevels = [0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0];
   this.zoomIndex = 3;  // Start at 1.0 (fit)

   var self = this;

   this.scrollbox = new ScrollBox(this);
   this.scrollbox.autoScroll = true;
   this.scrollbox.tracking = true;

   this.scrollbox.onHorizontalScrollPosUpdated = function(newPos) {
      this.viewport.update();
   };

   this.scrollbox.onVerticalScrollPosUpdated = function(newPos) {
      this.viewport.update();
   };

   this.scrollbox.viewport.onPaint = function(x0, y0, x1, y1) {
      var graphics = new VectorGraphics(this);

      graphics.fillRect(x0, y0, x1, y1, new Brush(0xff202020));

      if (self.scaledImage) {
         var offsetX = (this.width - self.scaledImage.width) / 2;
         var offsetY = (this.height - self.scaledImage.height) / 2;
         if (offsetX < 0) offsetX = 0;
         if (offsetY < 0) offsetY = 0;

         graphics.translateTransformation(offsetX, offsetY);
         graphics.drawBitmap(0, 0, self.scaledImage);

         // Draw border around preview
         graphics.pen = new Pen(0xffffffff, 1);
         var bw = self.scaledImage.width;
         var bh = self.scaledImage.height;
         graphics.drawLine(-1, -1, bw, -1);
         graphics.drawLine(bw, -1, bw, bh);
         graphics.drawLine(bw, bh, -1, bh);
         graphics.drawLine(-1, bh, -1, -1);

         if (self.previewMode === 2) {
            var splitX = Math.round(self.scaledImage.width * self.splitPosition / 100);
            graphics.pen = new Pen(0xaaffffff, 2);
            graphics.drawLine(splitX, 0, splitX, self.scaledImage.height);
         }

         graphics.antialiasing = true;
         graphics.pen = new Pen(0xffffffff);
         if (self.previewMode === 2) {
            graphics.drawText(5, 15, "BEFORE");
            graphics.drawText(self.scaledImage.width - 45, 15, "AFTER");
         } else if (self.previewMode === 1) {
            graphics.drawText(5, 15, "BEFORE");
         } else {
            graphics.drawText(5, 15, "AFTER");
         }
      } else {
         graphics.pen = new Pen(0xff888888);
         graphics.drawText(this.width / 2 - 40, this.height / 2, "No preview");
      }

      graphics.end();
   };

   this.scrollbox.viewport.onResize = function(wNew, hNew, wOld, hOld) {
      self.regenerate();
   };

   this.setMinSize(200, 150);

   this.sizer = new VerticalSizer;
   this.sizer.add(this.scrollbox, 100);

   this.regenerate = function() {
      if (!this.sourceWindow || !this.engine || !this.engine.parameters) {
         this.scaledImage = null;
         this.scrollbox.viewport.update();
         return;
      }

      var vpWidth = this.scrollbox.viewport.width;
      var vpHeight = this.scrollbox.viewport.height;

      if (vpWidth <= 0 || vpHeight <= 0) return;

      // Calculate preview size preserving aspect ratio
      var image = this.sourceWindow.mainView.image;
      if (!image) return;

      var imgW = image.width;
      var imgH = image.height;

      var outW, outH;
      if (this.zoomFactor <= 1.0) {
         // Fit mode: scale to fit viewport
         var maxW = vpWidth - 20;
         var maxH = vpHeight - 20;
         var scale = Math.min(maxW / imgW, maxH / imgH) * this.zoomFactor;
         outW = Math.max(1, Math.round(imgW * scale));
         outH = Math.max(1, Math.round(imgH * scale));
      } else {
         // Zoom mode: scale beyond fit
         var maxW = vpWidth - 20;
         var maxH = vpHeight - 20;
         var fitScale = Math.min(maxW / imgW, maxH / imgH);
         var scale = fitScale * this.zoomFactor;
         outW = Math.max(1, Math.round(imgW * scale));
         outH = Math.max(1, Math.round(imgH * scale));
      }

      this.scaledImage = this.engine.generatePreviewAtSize(
         this.sourceWindow, outW, outH, this.previewMode, this.splitPosition
      );

      this.scrollbox.viewport.update();
   };

   this.updatePreview = function() {
      this.regenerate();
   };

   this.zoomIn = function() {
      if (this.zoomIndex < this.zoomLevels.length - 1) {
         this.zoomIndex++;
         this.zoomFactor = this.zoomLevels[this.zoomIndex];
         this.regenerate();
      }
   };

   this.zoomOut = function() {
      if (this.zoomIndex > 0) {
         this.zoomIndex--;
         this.zoomFactor = this.zoomLevels[this.zoomIndex];
         this.regenerate();
      }
   };

   this.zoomToFit = function() {
      this.zoomIndex = 3;
      this.zoomFactor = 1.0;
      this.regenerate();
   };

   this.getZoomText = function() {
      return Math.round(this.zoomFactor * 100) + "%";
   };
}

PreviewControl.prototype = new Frame;

// ============================================================================
// RNCColorStretchDialog - Main dialog
// ============================================================================
function RNCColorStretchDialog(parameters, engine) {
   this.__base__ = Dialog;
   this.__base__();

   this.parameters = parameters;
   this.engine = engine;
   this.targetView = null;
   this.showPreview = true;

   // Throttling state for slider responsiveness
   this.lastPreviewTime = 0;
   this.previewSkipCount = 0;

   var self = this;

   this.windowTitle = TITLE + " v" + VERSION;
   this.minWidth = 750;
   this.minHeight = 600;

   // Target image selection
   this.targetImage_Label = new Label(this);
   this.targetImage_Label.text = "Target:";
   this.targetImage_Label.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.targetImage_Label.minWidth = 60;

   this.targetImage_ViewList = new ViewList(this);
   this.targetImage_ViewList.getMainViews();
   this.targetImage_ViewList.onViewSelected = function(view) {
      self.targetView = view;
      if (self.previewControl) {
         self.previewControl.sourceWindow = view && !view.isNull ? view.window : null;
         self.schedulePreviewUpdate();
      }
   };

   var activeWindow = ImageWindow.activeWindow;
   if (!activeWindow.isNull) {
      this.targetView = activeWindow.mainView;
      this.targetImage_ViewList.currentView = this.targetView;
   }

   this.targetImage_Sizer = new HorizontalSizer;
   this.targetImage_Sizer.spacing = 6;
   this.targetImage_Sizer.add(this.targetImage_Label);
   this.targetImage_Sizer.add(this.targetImage_ViewList, 100);

   // Linear input warning
   this.linearWarning_Label = new Label(this);
   this.linearWarning_Label.text = "Note: This algorithm is designed for LINEAR (unstretched) images.";
   this.linearWarning_Label.textAlignment = TextAlign_Left | TextAlign_VertCenter;
   this.linearWarning_Label.styleSheet = "QLabel { color: #e0a000; font-style: italic; }";

   this.targetImage_GroupBox = new GroupBox(this);
   this.targetImage_GroupBox.title = "Input Image";
   this.targetImage_GroupBox.sizer = new VerticalSizer;
   this.targetImage_GroupBox.sizer.margin = 8;
   this.targetImage_GroupBox.sizer.spacing = 4;
   this.targetImage_GroupBox.sizer.add(this.targetImage_Sizer);
   this.targetImage_GroupBox.sizer.add(this.linearWarning_Label);

   // -------------------------------------------------------------------------
   // Preview Control
   // -------------------------------------------------------------------------
   this.previewControl = new PreviewControl(this, engine);
   this.previewControl.setMinSize(350, 250);
   if (this.targetView && !this.targetView.isNull) {
      this.previewControl.sourceWindow = this.targetView.window;
   }

   // Preview mode buttons
   this.previewAfter_RadioButton = new RadioButton(this);
   this.previewAfter_RadioButton.text = "After";
   this.previewAfter_RadioButton.checked = true;
   this.previewAfter_RadioButton.onCheck = function(checked) {
      if (checked) {
         self.previewControl.previewMode = 0;
         self.schedulePreviewUpdate();
      }
   };

   this.previewBefore_RadioButton = new RadioButton(this);
   this.previewBefore_RadioButton.text = "Before";
   this.previewBefore_RadioButton.checked = false;
   this.previewBefore_RadioButton.onCheck = function(checked) {
      if (checked) {
         self.previewControl.previewMode = 1;
         self.schedulePreviewUpdate();
      }
   };

   this.previewSplit_RadioButton = new RadioButton(this);
   this.previewSplit_RadioButton.text = "Split";
   this.previewSplit_RadioButton.checked = false;
   this.previewSplit_RadioButton.onCheck = function(checked) {
      if (checked) {
         self.previewControl.previewMode = 2;
         self.schedulePreviewUpdate();
      }
   };

   // Zoom buttons
   this.zoomOut_Button = new ToolButton(this);
   this.zoomOut_Button.icon = this.scaledResource(":/icons/zoom-out.png");
   this.zoomOut_Button.setScaledFixedSize(24, 24);
   this.zoomOut_Button.toolTip = "Zoom Out";
   this.zoomOut_Button.onClick = function() {
      self.previewControl.zoomOut();
      self.zoomLabel.text = self.previewControl.getZoomText();
   };

   this.zoomIn_Button = new ToolButton(this);
   this.zoomIn_Button.icon = this.scaledResource(":/icons/zoom-in.png");
   this.zoomIn_Button.setScaledFixedSize(24, 24);
   this.zoomIn_Button.toolTip = "Zoom In";
   this.zoomIn_Button.onClick = function() {
      self.previewControl.zoomIn();
      self.zoomLabel.text = self.previewControl.getZoomText();
   };

   this.zoomFit_Button = new ToolButton(this);
   this.zoomFit_Button.icon = this.scaledResource(":/icons/zoom-optimal-fit.png");
   this.zoomFit_Button.setScaledFixedSize(24, 24);
   this.zoomFit_Button.toolTip = "Zoom to Fit";
   this.zoomFit_Button.onClick = function() {
      self.previewControl.zoomToFit();
      self.zoomLabel.text = self.previewControl.getZoomText();
   };

   this.zoomLabel = new Label(this);
   this.zoomLabel.text = "100%";
   this.zoomLabel.textAlignment = TextAlign_Center | TextAlign_VertCenter;
   this.zoomLabel.setFixedWidth(50);

   this.previewMode_Sizer = new HorizontalSizer;
   this.previewMode_Sizer.spacing = 6;
   this.previewMode_Sizer.add(this.previewAfter_RadioButton);
   this.previewMode_Sizer.add(this.previewBefore_RadioButton);
   this.previewMode_Sizer.add(this.previewSplit_RadioButton);
   this.previewMode_Sizer.addStretch();
   this.previewMode_Sizer.add(this.zoomOut_Button);
   this.previewMode_Sizer.add(this.zoomLabel);
   this.previewMode_Sizer.add(this.zoomIn_Button);
   this.previewMode_Sizer.add(this.zoomFit_Button);

   this.preview_GroupBox = new GroupBox(this);
   this.preview_GroupBox.title = "Preview";
   this.preview_GroupBox.sizer = new VerticalSizer;
   this.preview_GroupBox.sizer.margin = 8;
   this.preview_GroupBox.sizer.spacing = 6;
   this.preview_GroupBox.sizer.add(this.previewMode_Sizer);
   this.preview_GroupBox.sizer.add(this.previewControl, 100);

   // Root Power
   this.rootpower_Control = new NumericControl(this);
   this.rootpower_Control.label.text = "Root Power:";
   this.rootpower_Control.label.minWidth = 100;
   this.rootpower_Control.setRange(1.0, 200.0);
   this.rootpower_Control.slider.setRange(0, 1000);
   this.rootpower_Control.slider.scaledMinWidth = 180;
   this.rootpower_Control.setPrecision(1);
   this.rootpower_Control.setValue(parameters.rootpower);
   this.rootpower_Control.toolTip = "<p>Primary power stretch factor. Higher = stronger lift of faint areas.</p>";
   this.rootpower_Control.onValueUpdated = function(value) {
      self.parameters.rootpower = value;
      self.schedulePreviewUpdate();
   };

   // Two-Pass checkbox
   this.twoPass_CheckBox = new CheckBox(this);
   this.twoPass_CheckBox.text = "Two-Pass Stretch";
   this.twoPass_CheckBox.checked = parameters.twoPass;
   this.twoPass_CheckBox.onCheck = function(checked) {
      self.parameters.twoPass = checked;
      self.rootpower2_Control.enabled = checked;
      self.schedulePreviewUpdate();
   };

   // Root Power 2
   this.rootpower2_Control = new NumericControl(this);
   this.rootpower2_Control.label.text = "Root Power 2:";
   this.rootpower2_Control.label.minWidth = 100;
   this.rootpower2_Control.setRange(1.0, 20.0);
   this.rootpower2_Control.slider.setRange(0, 200);
   this.rootpower2_Control.slider.scaledMinWidth = 180;
   this.rootpower2_Control.setPrecision(1);
   this.rootpower2_Control.setValue(parameters.rootpower2);
   this.rootpower2_Control.enabled = parameters.twoPass;
   this.rootpower2_Control.onValueUpdated = function(value) {
      self.parameters.rootpower2 = value;
      self.schedulePreviewUpdate();
   };

   // S-Curve selection
   this.scurve_Label = new Label(this);
   this.scurve_Label.text = "S-Curve:";
   this.scurve_Label.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.scurve_Label.minWidth = 100;

   this.scurveNone_RadioButton = new RadioButton(this);
   this.scurveNone_RadioButton.text = "None";
   this.scurveNone_RadioButton.checked = (parameters.scurve === 0);
   this.scurveNone_RadioButton.onCheck = function(checked) {
      if (checked) { self.parameters.scurve = 0; self.schedulePreviewUpdate(); }
   };

   this.scurve1_RadioButton = new RadioButton(this);
   this.scurve1_RadioButton.text = "SC1";
   this.scurve1_RadioButton.checked = (parameters.scurve === 1);
   this.scurve1_RadioButton.onCheck = function(checked) {
      if (checked) { self.parameters.scurve = 1; self.schedulePreviewUpdate(); }
   };

   this.scurve2_RadioButton = new RadioButton(this);
   this.scurve2_RadioButton.text = "SC2";
   this.scurve2_RadioButton.checked = (parameters.scurve === 2);
   this.scurve2_RadioButton.onCheck = function(checked) {
      if (checked) { self.parameters.scurve = 2; self.schedulePreviewUpdate(); }
   };

   this.scurve3_RadioButton = new RadioButton(this);
   this.scurve3_RadioButton.text = "SC3";
   this.scurve3_RadioButton.checked = (parameters.scurve === 3);
   this.scurve3_RadioButton.onCheck = function(checked) {
      if (checked) { self.parameters.scurve = 3; self.schedulePreviewUpdate(); }
   };

   this.scurve4_RadioButton = new RadioButton(this);
   this.scurve4_RadioButton.text = "SC4";
   this.scurve4_RadioButton.checked = (parameters.scurve === 4);
   this.scurve4_RadioButton.onCheck = function(checked) {
      if (checked) { self.parameters.scurve = 4; self.schedulePreviewUpdate(); }
   };

   this.scurve_Sizer = new HorizontalSizer;
   this.scurve_Sizer.spacing = 8;
   this.scurve_Sizer.add(this.scurve_Label);
   this.scurve_Sizer.add(this.scurveNone_RadioButton);
   this.scurve_Sizer.add(this.scurve1_RadioButton);
   this.scurve_Sizer.add(this.scurve2_RadioButton);
   this.scurve_Sizer.add(this.scurve3_RadioButton);
   this.scurve_Sizer.add(this.scurve4_RadioButton);
   this.scurve_Sizer.addStretch();

   this.stretchParams_GroupBox = new GroupBox(this);
   this.stretchParams_GroupBox.title = "Stretch Parameters";
   this.stretchParams_GroupBox.sizer = new VerticalSizer;
   this.stretchParams_GroupBox.sizer.margin = 8;
   this.stretchParams_GroupBox.sizer.spacing = 6;
   this.stretchParams_GroupBox.sizer.add(this.rootpower_Control);
   this.stretchParams_GroupBox.sizer.add(this.twoPass_CheckBox);
   this.stretchParams_GroupBox.sizer.add(this.rootpower2_Control);
   this.stretchParams_GroupBox.sizer.add(this.scurve_Sizer);

   // Sky Level Factor
   this.skylevelfactor_Control = new NumericControl(this);
   this.skylevelfactor_Control.label.text = "Sky Level Factor:";
   this.skylevelfactor_Control.label.minWidth = 100;
   this.skylevelfactor_Control.setRange(0.001, 0.5);
   this.skylevelfactor_Control.slider.setRange(0, 1000);
   this.skylevelfactor_Control.slider.scaledMinWidth = 180;
   this.skylevelfactor_Control.setPrecision(4);
   this.skylevelfactor_Control.setValue(parameters.skylevelfactor);
   this.skylevelfactor_Control.onValueUpdated = function(value) {
      self.parameters.skylevelfactor = value;
      self.schedulePreviewUpdate();
   };

   // Link RGB checkbox
   this.linkSkyZero_CheckBox = new CheckBox(this);
   this.linkSkyZero_CheckBox.text = "Link RGB Sky Zero";
   this.linkSkyZero_CheckBox.checked = parameters.linkSkyZero;
   this.linkSkyZero_CheckBox.onCheck = function(checked) {
      self.parameters.linkSkyZero = checked;
      self.skyZero_Control.visible = checked;
      self.skyZeroRGB_Sizer.visible = !checked;
      self.schedulePreviewUpdate();
   };

   // Sky Zero (linked)
   this.skyZero_Control = new NumericControl(this);
   this.skyZero_Control.label.text = "Sky Zero (16-bit):";
   this.skyZero_Control.label.minWidth = 100;
   this.skyZero_Control.setRange(0, 16384);
   this.skyZero_Control.slider.setRange(0, 1000);
   this.skyZero_Control.slider.scaledMinWidth = 180;
   this.skyZero_Control.setPrecision(0);
   this.skyZero_Control.setValue(parameters.rgbskyzero[0]);
   this.skyZero_Control.visible = parameters.linkSkyZero;
   this.skyZero_Control.onValueUpdated = function(value) {
      var v = Math.round(value);
      self.parameters.rgbskyzero = [v, v, v];
      self.schedulePreviewUpdate();
   };

   // Sky Zero R/G/B
   this.skyZeroR_SpinBox = new SpinBox(this);
   this.skyZeroR_SpinBox.minValue = 0;
   this.skyZeroR_SpinBox.maxValue = 65535;
   this.skyZeroR_SpinBox.value = parameters.rgbskyzero[0];
   this.skyZeroR_SpinBox.onValueUpdated = function(value) {
      self.parameters.rgbskyzero[0] = value;
      self.schedulePreviewUpdate();
   };

   this.skyZeroG_SpinBox = new SpinBox(this);
   this.skyZeroG_SpinBox.minValue = 0;
   this.skyZeroG_SpinBox.maxValue = 65535;
   this.skyZeroG_SpinBox.value = parameters.rgbskyzero[1];
   this.skyZeroG_SpinBox.onValueUpdated = function(value) {
      self.parameters.rgbskyzero[1] = value;
      self.schedulePreviewUpdate();
   };

   this.skyZeroB_SpinBox = new SpinBox(this);
   this.skyZeroB_SpinBox.minValue = 0;
   this.skyZeroB_SpinBox.maxValue = 65535;
   this.skyZeroB_SpinBox.value = parameters.rgbskyzero[2];
   this.skyZeroB_SpinBox.onValueUpdated = function(value) {
      self.parameters.rgbskyzero[2] = value;
      self.schedulePreviewUpdate();
   };

   var skyZeroR_Label = new Label(this);
   skyZeroR_Label.text = "R:";
   var skyZeroG_Label = new Label(this);
   skyZeroG_Label.text = "G:";
   var skyZeroB_Label = new Label(this);
   skyZeroB_Label.text = "B:";

   this.skyZeroRGB_Sizer = new HorizontalSizer;
   this.skyZeroRGB_Sizer.spacing = 4;
   this.skyZeroRGB_Sizer.addSpacing(104);
   this.skyZeroRGB_Sizer.add(skyZeroR_Label);
   this.skyZeroRGB_Sizer.add(this.skyZeroR_SpinBox);
   this.skyZeroRGB_Sizer.addSpacing(8);
   this.skyZeroRGB_Sizer.add(skyZeroG_Label);
   this.skyZeroRGB_Sizer.add(this.skyZeroG_SpinBox);
   this.skyZeroRGB_Sizer.addSpacing(8);
   this.skyZeroRGB_Sizer.add(skyZeroB_Label);
   this.skyZeroRGB_Sizer.add(this.skyZeroB_SpinBox);
   this.skyZeroRGB_Sizer.addStretch();
   this.skyZeroRGB_Sizer.visible = !parameters.linkSkyZero;

   // Auto Detect button
   this.autoDetect_Button = new PushButton(this);
   this.autoDetect_Button.text = "Auto Detect Sky";
   this.autoDetect_Button.onClick = function() {
      if (self.targetView && !self.targetView.isNull) {
         self.engine.parameters = self.parameters;
         var skyLevels = self.engine.autoDetectSky(self.targetView.image);
         if (self.parameters.linkSkyZero) {
            var avg = Math.round((skyLevels[0] + skyLevels[1] + skyLevels[2]) / 3);
            self.parameters.rgbskyzero = [avg, avg, avg];
            self.skyZero_Control.setValue(avg);
         } else {
            self.parameters.rgbskyzero = skyLevels;
            self.skyZeroR_SpinBox.value = skyLevels[0];
            self.skyZeroG_SpinBox.value = skyLevels[1];
            self.skyZeroB_SpinBox.value = skyLevels[2];
         }
      }
   };

   this.skyButtons_Sizer = new HorizontalSizer;
   this.skyButtons_Sizer.addSpacing(104);
   this.skyButtons_Sizer.add(this.autoDetect_Button);
   this.skyButtons_Sizer.addStretch();

   this.skyCalibration_GroupBox = new GroupBox(this);
   this.skyCalibration_GroupBox.title = "Sky Calibration";
   this.skyCalibration_GroupBox.sizer = new VerticalSizer;
   this.skyCalibration_GroupBox.sizer.margin = 8;
   this.skyCalibration_GroupBox.sizer.spacing = 6;
   this.skyCalibration_GroupBox.sizer.add(this.skylevelfactor_Control);
   this.skyCalibration_GroupBox.sizer.add(this.linkSkyZero_CheckBox);
   this.skyCalibration_GroupBox.sizer.add(this.skyZero_Control);
   this.skyCalibration_GroupBox.sizer.add(this.skyZeroRGB_Sizer);
   this.skyCalibration_GroupBox.sizer.add(this.skyButtons_Sizer);

   // Color Recovery
   this.colorRecovery_CheckBox = new CheckBox(this);
   this.colorRecovery_CheckBox.text = "Enable Color Recovery";
   this.colorRecovery_CheckBox.checked = parameters.colorRecovery;
   this.colorRecovery_CheckBox.onCheck = function(checked) {
      self.parameters.colorRecovery = checked;
      self.colorLimit_Control.enabled = checked;
      self.enhance_Control.enabled = checked;
      self.schedulePreviewUpdate();
   };

   this.colorLimit_Control = new NumericControl(this);
   this.colorLimit_Control.label.text = "Color Ratio Limit:";
   this.colorLimit_Control.label.minWidth = 100;
   this.colorLimit_Control.setRange(0.1, 1.0);
   this.colorLimit_Control.slider.setRange(0, 100);
   this.colorLimit_Control.slider.scaledMinWidth = 180;
   this.colorLimit_Control.setPrecision(2);
   this.colorLimit_Control.setValue(parameters.colorLimit);
   this.colorLimit_Control.enabled = parameters.colorRecovery;
   this.colorLimit_Control.onValueUpdated = function(value) {
      self.parameters.colorLimit = value;
      self.schedulePreviewUpdate();
   };

   this.enhance_Control = new NumericControl(this);
   this.enhance_Control.label.text = "Enhancement:";
   this.enhance_Control.label.minWidth = 100;
   this.enhance_Control.setRange(0.5, 2.0);
   this.enhance_Control.slider.setRange(0, 150);
   this.enhance_Control.slider.scaledMinWidth = 180;
   this.enhance_Control.setPrecision(2);
   this.enhance_Control.setValue(parameters.enhance);
   this.enhance_Control.enabled = parameters.colorRecovery;
   this.enhance_Control.onValueUpdated = function(value) {
      self.parameters.enhance = value;
      self.schedulePreviewUpdate();
   };

   this.colorRecovery_GroupBox = new GroupBox(this);
   this.colorRecovery_GroupBox.title = "Color Recovery";
   this.colorRecovery_GroupBox.sizer = new VerticalSizer;
   this.colorRecovery_GroupBox.sizer.margin = 8;
   this.colorRecovery_GroupBox.sizer.spacing = 6;
   this.colorRecovery_GroupBox.sizer.add(this.colorRecovery_CheckBox);
   this.colorRecovery_GroupBox.sizer.add(this.colorLimit_Control);
   this.colorRecovery_GroupBox.sizer.add(this.enhance_Control);

   // -------------------------------------------------------------------------
   // Options Group Box
   // -------------------------------------------------------------------------
   this.createNewImage_CheckBox = new CheckBox(this);
   this.createNewImage_CheckBox.text = "Create new image";
   this.createNewImage_CheckBox.checked = parameters.createNewImage;
   this.createNewImage_CheckBox.onCheck = function(checked) {
      self.parameters.createNewImage = checked;
   };

   this.showPreview_CheckBox = new CheckBox(this);
   this.showPreview_CheckBox.text = "Show preview";
   this.showPreview_CheckBox.checked = this.showPreview;
   this.showPreview_CheckBox.onCheck = function(checked) {
      self.showPreview = checked;
      self.preview_GroupBox.visible = checked;
      if (checked) {
         self.schedulePreviewUpdate();
      }
   };

   this.options_GroupBox = new GroupBox(this);
   this.options_GroupBox.title = "Options";
   this.options_GroupBox.sizer = new VerticalSizer;
   this.options_GroupBox.sizer.margin = 8;
   this.options_GroupBox.sizer.spacing = 4;
   this.options_GroupBox.sizer.add(this.createNewImage_CheckBox);
   this.options_GroupBox.sizer.add(this.showPreview_CheckBox);

   // -------------------------------------------------------------------------
   // Buttons (in left panel)
   // -------------------------------------------------------------------------
   this.newInstance_Button = new ToolButton(this);
   this.newInstance_Button.icon = this.scaledResource(":/process-interface/new-instance.png");
   this.newInstance_Button.setScaledFixedSize(24, 24);
   this.newInstance_Button.toolTip = "New Instance";
   this.newInstance_Button.onMousePress = function() {
      self.parameters.exportParameters();
      self.newInstance();
   };

   this.reset_Button = new PushButton(this);
   this.reset_Button.text = "Reset";
   this.reset_Button.onClick = function() {
      self.parameters.reset();
      self.updateControls();
   };

   this.ok_Button = new PushButton(this);
   this.ok_Button.text = "Execute";
   this.ok_Button.icon = this.scaledResource(":/icons/ok.png");
   this.ok_Button.onClick = function() {
      if (self.executeProcess()) {
         self.ok();
      }
   };

   this.cancel_Button = new PushButton(this);
   this.cancel_Button.text = "Cancel";
   this.cancel_Button.icon = this.scaledResource(":/icons/cancel.png");
   this.cancel_Button.onClick = function() {
      self.cancel();
   };

   this.buttons_Sizer = new HorizontalSizer;
   this.buttons_Sizer.spacing = 8;
   this.buttons_Sizer.add(this.newInstance_Button);
   this.buttons_Sizer.addSpacing(8);
   this.buttons_Sizer.add(this.reset_Button);
   this.buttons_Sizer.addStretch();
   this.buttons_Sizer.add(this.cancel_Button);
   this.buttons_Sizer.add(this.ok_Button);

   // -------------------------------------------------------------------------
   // Layout
   // -------------------------------------------------------------------------
   // Left panel (controls + buttons)
   this.leftPanel_Sizer = new VerticalSizer;
   this.leftPanel_Sizer.spacing = 6;
   this.leftPanel_Sizer.add(this.targetImage_GroupBox);
   this.leftPanel_Sizer.add(this.stretchParams_GroupBox);
   this.leftPanel_Sizer.add(this.skyCalibration_GroupBox);
   this.leftPanel_Sizer.add(this.colorRecovery_GroupBox);
   this.leftPanel_Sizer.add(this.options_GroupBox);
   this.leftPanel_Sizer.addStretch();
   this.leftPanel_Sizer.add(this.buttons_Sizer);

   // Content area (left controls + right preview)
   this.content_Sizer = new HorizontalSizer;
   this.content_Sizer.spacing = 8;
   this.content_Sizer.add(this.leftPanel_Sizer);
   this.content_Sizer.add(this.preview_GroupBox, 100);

   // Main layout
   this.sizer = new VerticalSizer;
   this.sizer.margin = 8;
   this.sizer.spacing = 8;
   this.sizer.add(this.content_Sizer, 100);

   this.adjustToContents();

   // Initial preview update
   if (this.targetView && !this.targetView.isNull) {
      this.schedulePreviewUpdate();
   }
}

RNCColorStretchDialog.prototype = new Dialog;

RNCColorStretchDialog.prototype.schedulePreviewUpdate = function() {
   if (!this.showPreview) return;
   if (!this.previewControl) return;

   // Throttle updates - skip if last update was less than 80ms ago
   var now = new Date().getTime();
   var elapsed = now - this.lastPreviewTime;

   if (elapsed < 80) {
      // Skip this update but count it
      this.previewSkipCount++;
      // Only force update every 4th skip to keep preview somewhat responsive
      if (this.previewSkipCount < 4) {
         return;
      }
   }

   this.previewSkipCount = 0;
   this.lastPreviewTime = now;

   // Update engine parameters and refresh preview
   this.engine.parameters = this.parameters;
   this.previewControl.updatePreview();
};

RNCColorStretchDialog.prototype.forcePreviewUpdate = function() {
   // Bypass throttling for button clicks
   this.lastPreviewTime = 0;
   this.previewSkipCount = 0;
   this.engine.parameters = this.parameters;
   this.previewControl.updatePreview();
};

RNCColorStretchDialog.prototype.updateControls = function() {
   var P = this.parameters;
   this.rootpower_Control.setValue(P.rootpower);
   this.twoPass_CheckBox.checked = P.twoPass;
   this.rootpower2_Control.setValue(P.rootpower2);
   this.rootpower2_Control.enabled = P.twoPass;
   this.scurveNone_RadioButton.checked = (P.scurve === 0);
   this.scurve1_RadioButton.checked = (P.scurve === 1);
   this.scurve2_RadioButton.checked = (P.scurve === 2);
   this.scurve3_RadioButton.checked = (P.scurve === 3);
   this.scurve4_RadioButton.checked = (P.scurve === 4);
   this.skylevelfactor_Control.setValue(P.skylevelfactor);
   this.linkSkyZero_CheckBox.checked = P.linkSkyZero;
   this.skyZero_Control.setValue(P.rgbskyzero[0]);
   this.skyZero_Control.visible = P.linkSkyZero;
   this.skyZeroRGB_Sizer.visible = !P.linkSkyZero;
   this.skyZeroR_SpinBox.value = P.rgbskyzero[0];
   this.skyZeroG_SpinBox.value = P.rgbskyzero[1];
   this.skyZeroB_SpinBox.value = P.rgbskyzero[2];
   this.colorRecovery_CheckBox.checked = P.colorRecovery;
   this.colorLimit_Control.setValue(P.colorLimit);
   this.colorLimit_Control.enabled = P.colorRecovery;
   this.enhance_Control.setValue(P.enhance);
   this.enhance_Control.enabled = P.colorRecovery;
   this.createNewImage_CheckBox.checked = P.createNewImage;
   this.schedulePreviewUpdate();
};

RNCColorStretchDialog.prototype.executeProcess = function() {
   if (!this.targetView || this.targetView.isNull) {
      var msgBox = new MessageBox(
         "Please select a target image.",
         TITLE,
         StdIcon_Warning,
         StdButton_Ok
      );
      msgBox.execute();
      return false;
   }

   var targetImage = this.targetView.image;

   if (targetImage.numberOfChannels < 3) {
      var msgBox = new MessageBox(
         "RNC-ColorStretch requires an RGB color image.",
         TITLE,
         StdIcon_Error,
         StdButton_Ok
      );
      msgBox.execute();
      return false;
   }

   this.parameters.validate();
   this.engine.parameters = this.parameters;

   console.show();
   console.writeln("<b>" + TITLE + " v" + VERSION + "</b>");
   console.writeln("Processing: " + this.targetView.fullId);
   console.flush();

   var startTime = Date.now();

   try {
      if (this.parameters.createNewImage) {
         var newWindow = new ImageWindow(
            targetImage.width,
            targetImage.height,
            targetImage.numberOfChannels,
            targetImage.bitsPerSample,
            targetImage.isReal,
            targetImage.isColor,
            this.targetView.id + "_RNC"
         );
         var newView = newWindow.mainView;

         newView.beginProcess(UndoFlag_NoSwapFile);
         newView.image.assign(targetImage);
         this.engine.apply(newView.image, false);
         newView.endProcess();

         newWindow.show();
      } else {
         this.targetView.beginProcess(UndoFlag_PixelData);
         this.engine.apply(targetImage, false);
         this.targetView.endProcess();
      }

      var elapsed = (Date.now() - startTime) / 1000;
      console.writeln("Processing completed in " + elapsed.toFixed(2) + " seconds.");

   } catch (error) {
      console.criticalln("Error: " + error.message);
      var msgBox = new MessageBox(
         "Processing failed: " + error.message,
         TITLE,
         StdIcon_Error,
         StdButton_Ok
      );
      msgBox.execute();
      return false;
   }

   return true;
};

// ============================================================================
// Script entry point
// ============================================================================
function main() {
   console.hide();

   var parameters = new RNCColorStretchParameters();
   parameters.load();

   if (Parameters.isViewTarget) {
      parameters.importParameters();
   }

   var engine = new RNCColorStretchEngine();
   engine.parameters = parameters;

   if (Parameters.isViewTarget) {
      var targetView = Parameters.targetView;

      if (targetView.isNull) {
         console.criticalln("No target view specified.");
         return;
      }

      if (targetView.image.numberOfChannels < 3) {
         console.criticalln("RNC-ColorStretch requires an RGB color image.");
         return;
      }

      console.show();
      console.writeln("<b>" + TITLE + " v" + VERSION + "</b>");
      console.writeln("Processing: " + targetView.fullId);
      console.flush();

      var startTime = Date.now();

      try {
         if (parameters.createNewImage) {
            var targetImage = targetView.image;
            var newWindow = new ImageWindow(
               targetImage.width,
               targetImage.height,
               targetImage.numberOfChannels,
               targetImage.bitsPerSample,
               targetImage.isReal,
               targetImage.isColor,
               targetView.id + "_RNC"
            );
            var newView = newWindow.mainView;

            newView.beginProcess(UndoFlag_NoSwapFile);
            newView.image.assign(targetImage);
            engine.apply(newView.image, false);
            newView.endProcess();

            newWindow.show();
         } else {
            targetView.beginProcess(UndoFlag_PixelData);
            engine.apply(targetView.image, false);
            targetView.endProcess();
         }

         var elapsed = (Date.now() - startTime) / 1000;
         console.writeln("Processing completed in " + elapsed.toFixed(2) + " seconds.");

      } catch (error) {
         console.criticalln("Error: " + error.message);
      }

      return;
   }

   var dialog = new RNCColorStretchDialog(parameters, engine);

   while (true) {
      if (dialog.execute()) {
         parameters.save();
         break;
      } else {
         break;
      }
   }
}

main();
