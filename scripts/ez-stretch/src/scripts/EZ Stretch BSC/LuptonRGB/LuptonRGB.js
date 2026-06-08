// ============================================================================
// LuptonRGB.js - Lupton RGB Stretch for PixInsight
// ============================================================================
//
// Implements the Lupton et al. (2004) RGB stretch algorithm for creating
// color-preserving stretched images from high dynamic range astronomical data.
//
// Reference: Lupton, R. et al. (2004) "Preparing Red-Green-Blue Images from
// CCD Data" PASP 116:133-137
// https://ui.adsabs.harvard.edu/abs/2004PASP..116..133L
//
// ============================================================================

#feature-id    EZ Stretch BSC > LuptonRGB
#script-id     LuptonRGB
#feature-info  Lupton RGB Stretch - Color-preserving arcsinh stretch for \
               astronomical images. Based on Lupton et al. (2004) PASP 116:133. \
               Creates properly color-balanced RGB images from linear data \
               while preventing star color clipping.

#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/TextAlign.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/StdCursor.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/UndoFlag.jsh>
#include <pjsr/SampleType.jsh>
#include <pjsr/FontFamily.jsh>
#include <pjsr/Color.jsh>

// Version check - require PixInsight 1.8.0 or higher
#iflt __PI_VERSION__ 01.08.00
#error This script requires PixInsight 1.8.0 or higher.
#endif

#define VERSION "1.0.14"
#define TITLE   "Lupton RGB Stretch"

// Enable automatic garbage collection
var jsAutoGC = true;

// ============================================================================
// Math.asinh polyfill for ECMA 262-5 compatibility
// ============================================================================
if (typeof Math.asinh === 'undefined') {
   Math.asinh = function(x) {
      return Math.log(x + Math.sqrt(x * x + 1));
   };
}

// ============================================================================
// Algorithm Engine
// ============================================================================

function LuptonEngine()
{
   // Default parameters
   this.stretch = 5.0;        // Alpha: linear amplification factor (0.1 - 50.0)
   this.Q = 8.0;              // Q: softening parameter (0.1 - 30.0)
   this.blackPoint = 0.0;     // Linked black point (-0.1 - 0.5)
   this.blackR = 0.0;         // Per-channel black point R
   this.blackG = 0.0;         // Per-channel black point G
   this.blackB = 0.0;         // Per-channel black point B
   this.linkedChannels = true;
   this.saturation = 1.0;     // Saturation boost (0.5 - 2.0)
   this.clippingMode = 0;     // 0: Preserve Color, 1: Hard Clip, 2: Rescale

   // Image sources
   this.useActiveImage = true;
   this.imageR = null;
   this.imageG = null;
   this.imageB = null;

   // Arcsinh stretch function F(x)
   this.F = function(x, alpha, Q, minimum)
   {
      var val = x - minimum;
      if (val <= 0) return 0;
      return Math.asinh(alpha * Q * val) / Q;
   };

   // Process a single pixel (r, g, b values 0-1)
   this.processPixel = function(r, g, b)
   {
      var minR = this.linkedChannels ? this.blackPoint : this.blackR;
      var minG = this.linkedChannels ? this.blackPoint : this.blackG;
      var minB = this.linkedChannels ? this.blackPoint : this.blackB;

      // For the Lupton algorithm, we use a single "minimum" for intensity calculation
      // but can subtract per-channel minimums for the final scaling
      var minimum = (minR + minG + minB) / 3;

      // Step 1: Compute intensity
      var I = (r + g + b) / 3;

      // Step 2: Compute scale factor using arcsinh stretch
      var scale = 0;
      var epsilon = 1e-10;
      if (I > minimum + epsilon)
      {
         var FI = this.F(I, this.stretch, this.Q, minimum);
         scale = FI / (I - minimum);
      }

      // Step 3: Apply scale to each channel (with per-channel black point subtraction)
      var rOut = (r - minR) * scale;
      var gOut = (g - minG) * scale;
      var bOut = (b - minB) * scale;

      // Step 4: Apply saturation boost
      if (Math.abs(this.saturation - 1.0) > 1e-6)
      {
         var lum = (rOut + gOut + bOut) / 3;
         rOut = lum + (rOut - lum) * this.saturation;
         gOut = lum + (gOut - lum) * this.saturation;
         bOut = lum + (bOut - lum) * this.saturation;
      }

      // Step 5: Handle clipping based on mode
      switch (this.clippingMode)
      {
         case 0: // Preserve Color (Lupton)
            var maxVal = Math.max(rOut, gOut, bOut);
            if (maxVal > 1.0)
            {
               rOut /= maxVal;
               gOut /= maxVal;
               bOut /= maxVal;
            }
            break;
         case 1: // Hard Clip
            rOut = Math.min(1.0, Math.max(0, rOut));
            gOut = Math.min(1.0, Math.max(0, gOut));
            bOut = Math.min(1.0, Math.max(0, bOut));
            break;
         case 2: // Rescale - handled at image level
            break;
      }

      // Clamp negative values
      rOut = Math.max(0, rOut);
      gOut = Math.max(0, gOut);
      bOut = Math.max(0, bOut);

      return [rOut, gOut, bOut];
   };

   // Calculate auto black point for an image/channel
   this.calculateAutoBlackPoint = function(view, channel)
   {
      var image = view.image;
      if (!image) return 0;

      // Use ImageStatistics for reliable calculations
      try
      {
         // Simple approach: sample some pixels and find low percentile
         var samples = [];
         var step = Math.max(1, Math.floor(Math.sqrt(image.width * image.height / 10000)));

         for (var y = 0; y < image.height; y += step)
         {
            for (var x = 0; x < image.width; x += step)
            {
               samples.push(image.sample(x, y, channel));
            }
         }

         // Guard against empty samples
         if (samples.length === 0) return 0;

         // Sort and find 1st percentile
         samples.sort(function(a, b) { return a - b; });
         var idx = Math.floor(samples.length * 0.01);
         var lowVal = samples[idx];

         // Find median of low values for more robust estimate
         var lowSamples = samples.slice(0, Math.max(10, Math.floor(samples.length * 0.05)));
         var medianIdx = Math.floor(lowSamples.length / 2);
         var median = lowSamples[medianIdx];

         // Return slightly above the noise floor
         return Math.max(0, median * 0.9);
      }
      catch(e)
      {
         console.warningln("Auto black point calculation failed: " + e.message);
         return 0;
      }
   };

   // Process entire image (creates new window)
   this.execute = function(targetWindow)
   {
      var startTime = new Date().getTime();

      if (!targetWindow)
      {
         console.criticalln("Error: No target window specified");
         return null;
      }

      var sourceImage = targetWindow.mainView.image;
      if (sourceImage.numberOfChannels < 3)
      {
         console.criticalln("Error: Image must have at least 3 channels (RGB)");
         return null;
      }

      console.writeln("<b>Lupton RGB Stretch</b>");
      console.writeln("Processing: " + targetWindow.mainView.id);
      console.writeln(format("Parameters: alpha=%.2f, Q=%.2f", this.stretch, this.Q));

      var width = sourceImage.width;
      var height = sourceImage.height;

      // Create output window by cloning the source
      var outputId = targetWindow.mainView.id + "_lupton";
      var outputWindow = null;

      try
      {
         // Create output window
         outputWindow = new ImageWindow(
            width,
            height,
            3,
            32,  // 32-bit float
            true, // float
            true, // color
            outputId
         );

         // Copy source to output
         outputWindow.mainView.beginProcess(UndoFlag_NoSwapFile);
         try {
            outputWindow.mainView.image.apply(sourceImage);
         } finally {
            outputWindow.mainView.endProcess();
         }

         // Apply Lupton stretch using PixelMath (two-pass for reliability)
         var alpha = this.stretch;
         var Q = this.Q;

         // The Lupton formula:
         // I = (R+G+B)/3
         // F(x) = asinh(alpha*Q*(x-min))/Q
         // scale = F(I)/(I-min)
         // out = (in - min) * scale

         var minR = this.linkedChannels ? this.blackPoint : this.blackR;
         var minG = this.linkedChannels ? this.blackPoint : this.blackG;
         var minB = this.linkedChannels ? this.blackPoint : this.blackB;
         var avgMin = (minR + minG + minB) / 3;

         // Intensity calculation (same for all channels)
         var intensity = "($T[0]+$T[1]+$T[2])/3";
         var epsilon = 1e-10;

         // Prevent Q from being too close to zero (causes division issues)
         var safeQ = (Math.abs(Q) < 0.01) ? (Q >= 0 ? 0.01 : -0.01) : Q;

         // F(I) = asinh(alpha*Q*(I-min))/Q using ln(x + sqrt(x^2+1))
         var aQ = alpha * safeQ;
         var arg = aQ + "*(" + intensity + "-" + avgMin + ")";
         var FI = "ln(" + arg + "+sqrt(" + arg + "*" + arg + "+1))/" + safeQ;
         // Add protection against division by very small denominators
         var scale = "iif(" + intensity + ">" + (avgMin + epsilon) + "," + FI + "/max(" + epsilon + "," + intensity + "-" + avgMin + "),0)";

         // Wrap all PixelMath passes in a single process block for proper undo
         outputWindow.mainView.beginProcess(UndoFlag_NoSwapFile);
         try
         {
            // PASS 1: Apply Lupton stretch (no clipping yet)
            var P1 = new PixelMath;
            P1.expression = "max(0,($T[0]-" + minR + ")*" + scale + ")";
            P1.expression1 = "max(0,($T[1]-" + minG + ")*" + scale + ")";
            P1.expression2 = "max(0,($T[2]-" + minB + ")*" + scale + ")";
            P1.useSingleExpression = false;
            P1.createNewImage = false;
            P1.rescale = false;
            P1.truncate = false;  // Don't truncate yet - preserve values > 1

            console.writeln("Pass 1: Applying Lupton stretch...");
            if (!P1.executeOn(outputWindow.mainView))
               throw new Error("PixelMath pass 1 failed");

            // PASS 2: Apply saturation adjustment if needed
            if (Math.abs(this.saturation - 1.0) > 1e-6)
            {
               var sat = this.saturation;
               var P2 = new PixelMath;
               // lum = (R+G+B)/3, out = lum + (in - lum) * saturation
               var lum = "($T[0]+$T[1]+$T[2])/3";
               P2.expression = lum + "+($T[0]-" + lum + ")*" + sat;
               P2.expression1 = lum + "+($T[1]-" + lum + ")*" + sat;
               P2.expression2 = lum + "+($T[2]-" + lum + ")*" + sat;
               P2.useSingleExpression = false;
               P2.createNewImage = false;
               P2.rescale = false;
               P2.truncate = false;

               console.writeln("Pass 2: Applying saturation...");
               if (!P2.executeOn(outputWindow.mainView))
                  throw new Error("PixelMath pass 2 failed");
            }

            // PASS 3: Apply clipping based on mode
            var P3 = new PixelMath;
            if (this.clippingMode === 0)
            {
               // Preserve Color: divide all channels by max(R,G,B) if any > 1
               // This is the key Lupton feature - colors are preserved!
               var maxRGB = "max($T[0],max($T[1],$T[2]))";
               var clipScale = "iif(" + maxRGB + ">1,1/" + maxRGB + ",1)";
               P3.expression = "max(0,$T[0]*" + clipScale + ")";
               P3.expression1 = "max(0,$T[1]*" + clipScale + ")";
               P3.expression2 = "max(0,$T[2]*" + clipScale + ")";
               P3.rescale = false;
               P3.truncate = true;
               console.writeln("Pass 3: Applying color-preserving clip...");
            }
            else if (this.clippingMode === 1)
            {
               // Hard clip each channel independently
               P3.expression = "min(1,max(0,$T[0]))";
               P3.expression1 = "min(1,max(0,$T[1]))";
               P3.expression2 = "min(1,max(0,$T[2]))";
               P3.rescale = false;
               P3.truncate = true;
               console.writeln("Pass 3: Applying hard clip...");
            }
            else
            {
               // Rescale mode - let PixelMath handle it
               P3.expression = "$T[0]";
               P3.expression1 = "$T[1]";
               P3.expression2 = "$T[2]";
               P3.rescale = true;
               P3.truncate = true;
               console.writeln("Pass 3: Rescaling to fit...");
            }
            P3.useSingleExpression = false;
            P3.createNewImage = false;

            if (!P3.executeOn(outputWindow.mainView))
               throw new Error("PixelMath pass 3 failed");
         }
         finally
         {
            outputWindow.mainView.endProcess();
         }

         var elapsed = (new Date().getTime() - startTime) / 1000;
         console.writeln(format("Processing completed in %.2f seconds", elapsed));

         outputWindow.show();
         return outputWindow;
      }
      catch (e)
      {
         console.criticalln("Error during processing: " + e.message);
         if (outputWindow)
         {
            outputWindow.forceClose();
         }
         return null;
      }
   };

   // Generate preview bitmap
   this.generatePreview = function(sourceWindow, previewWidth, previewHeight, showBefore, splitPos, zoomLevel, panX, panY)
   {
      if (!sourceWindow) return null;

      var image = sourceWindow.mainView.image;
      if (!image || image.numberOfChannels < 3) return null;

      // Default zoom parameters
      if (zoomLevel === undefined) zoomLevel = 0;
      if (panX === undefined) panX = 0;
      if (panY === undefined) panY = 0;

      var imgWidth = image.width;
      var imgHeight = image.height;

      // Use actual preview size, capped at 800x600 for performance
      var maxPreviewW = Math.min(previewWidth, 800);
      var maxPreviewH = Math.min(previewHeight, 600);

      // Calculate scale based on zoom level
      var scale;
      var actualWidth, actualHeight;
      var offsetX = 0, offsetY = 0;

      if (zoomLevel === 0)
      {
         // Fit mode - scale to fit preview window (use limited size)
         var scaleX = imgWidth / maxPreviewW;
         var scaleY = imgHeight / maxPreviewH;
         scale = Math.max(scaleX, scaleY);
         actualWidth = Math.round(imgWidth / scale);
         actualHeight = Math.round(imgHeight / scale);
      }
      else
      {
         // Zoom mode - use zoom factor
         var zoomFactor = Math.pow(2, zoomLevel - 1); // level 1 = 100%, 2 = 200%, etc.
         scale = 1.0 / zoomFactor;

         // At 100% zoom, 1 image pixel = 1 preview pixel (but limit to maxPreview size)
         actualWidth = Math.min(maxPreviewW, Math.round(imgWidth * zoomFactor));
         actualHeight = Math.min(maxPreviewH, Math.round(imgHeight * zoomFactor));

         // Calculate visible region with pan offset
         offsetX = Math.max(0, Math.min(imgWidth - actualWidth / zoomFactor, panX));
         offsetY = Math.max(0, Math.min(imgHeight - actualHeight / zoomFactor, panY));
      }

      // Create bitmap
      var bitmap = new Bitmap(actualWidth, actualHeight);

      // Pre-calculate constants for the loop
      var splitX = actualWidth * splitPos / 100;

      for (var py = 0; py < actualHeight; py++)
      {
         var iy = (zoomLevel === 0)
            ? Math.min(Math.floor(py * scale), imgHeight - 1)
            : Math.min(Math.floor(offsetY + py * scale), imgHeight - 1);

         for (var px = 0; px < actualWidth; px++)
         {
            var ix = (zoomLevel === 0)
               ? Math.min(Math.floor(px * scale), imgWidth - 1)
               : Math.min(Math.floor(offsetX + px * scale), imgWidth - 1);

            // Get source pixel
            var r = image.sample(ix, iy, 0);
            var g = image.sample(ix, iy, 1);
            var b = image.sample(ix, iy, 2);

            var rOut, gOut, bOut;

            // Determine if this pixel is in "before" or "after" region
            var isBefore = (showBefore === 1) || (showBefore === 2 && px < splitX);

            if (isBefore)
            {
               // Show original (with basic STF-like stretch for visibility)
               rOut = Math.min(1, r * 10);
               gOut = Math.min(1, g * 10);
               bOut = Math.min(1, b * 10);
            }
            else
            {
               // Apply Lupton stretch
               var result = this.processPixel(r, g, b);
               rOut = result[0];
               gOut = result[1];
               bOut = result[2];
            }

            // Convert to 8-bit and create color (combined for speed)
            var r8 = (rOut > 1 ? 255 : (rOut < 0 ? 0 : (rOut * 255 + 0.5) | 0));
            var g8 = (gOut > 1 ? 255 : (gOut < 0 ? 0 : (gOut * 255 + 0.5) | 0));
            var b8 = (bOut > 1 ? 255 : (bOut < 0 ? 0 : (bOut * 255 + 0.5) | 0));

            bitmap.setPixel(px, py, 0xff000000 | (r8 << 16) | (g8 << 8) | b8);
         }
      }

      return bitmap;
   };

   // Generate preview at exact output size (for ScrollBox-based preview)
   this.generatePreviewAtSize = function(sourceWindow, outWidth, outHeight, showBefore, splitPos)
   {
      if (!sourceWindow) return null;
      if (outWidth <= 0 || outHeight <= 0) return null;

      var image = sourceWindow.mainView.image;
      if (!image || image.numberOfChannels < 3) return null;

      var imgWidth = image.width;
      var imgHeight = image.height;

      // Create bitmap at exact requested size
      var bitmap = new Bitmap(outWidth, outHeight);

      // Calculate sampling scale
      var scaleX = imgWidth / outWidth;
      var scaleY = imgHeight / outHeight;

      // Pre-calculate split position
      var splitX = outWidth * splitPos / 100;

      for (var py = 0; py < outHeight; py++)
      {
         var iy = Math.min(Math.floor(py * scaleY), imgHeight - 1);

         for (var px = 0; px < outWidth; px++)
         {
            var ix = Math.min(Math.floor(px * scaleX), imgWidth - 1);

            // Get source pixel
            var r = image.sample(ix, iy, 0);
            var g = image.sample(ix, iy, 1);
            var b = image.sample(ix, iy, 2);

            var rOut, gOut, bOut;

            // Determine if this pixel is in "before" or "after" region
            var isBefore = (showBefore === 1) || (showBefore === 2 && px < splitX);

            if (isBefore)
            {
               // Show original (with basic STF-like stretch for visibility)
               rOut = Math.min(1, r * 10);
               gOut = Math.min(1, g * 10);
               bOut = Math.min(1, b * 10);
            }
            else
            {
               // Apply Lupton stretch
               var result = this.processPixel(r, g, b);
               rOut = result[0];
               gOut = result[1];
               bOut = result[2];
            }

            // Convert to 8-bit and create color
            var r8 = (rOut > 1 ? 255 : (rOut < 0 ? 0 : (rOut * 255 + 0.5) | 0));
            var g8 = (gOut > 1 ? 255 : (gOut < 0 ? 0 : (gOut * 255 + 0.5) | 0));
            var b8 = (bOut > 1 ? 255 : (bOut < 0 ? 0 : (bOut * 255 + 0.5) | 0));

            bitmap.setPixel(px, py, 0xff000000 | (r8 << 16) | (g8 << 8) | b8);
         }
      }

      return bitmap;
   };

   // Reset to default values
   this.reset = function()
   {
      this.stretch = 5.0;
      this.Q = 8.0;
      this.blackPoint = 0.0;
      this.blackR = 0.0;
      this.blackG = 0.0;
      this.blackB = 0.0;
      this.linkedChannels = true;
      this.saturation = 1.0;
      this.clippingMode = 0;
   };
}

// ============================================================================
// Preview Control - Based on official PixInsight PJSR pattern (AdP PreviewControl)
// Uses Frame + ScrollBox + VectorGraphics for proper viewport rendering
// ============================================================================

function PreviewControl(parent, engine)
{
   this.__base__ = Frame;
   this.__base__(parent);

   this.engine = engine;
   this.bitmap = null;
   this.scaledImage = null;
   this.sourceWindow = null;
   this.previewMode = 0;  // 0: After, 1: Before, 2: Split
   this.splitPosition = 50;
   this.showCrosshair = false;

   // Zoom state
   this.zoom = 0;  // 0 = fit, 1 = 100%, 2 = 200%, -1 = 50%, etc.
   this.scale = 1.0;
   this.zoomOutLimit = -5;

   // For external compatibility
   this.zoomLevel = 0;
   this.zoomFactor = 1.0;
   this.panX = 0;
   this.panY = 0;

   var self = this;

   // ScrollBox setup - this is the key to proper PJSR preview rendering
   this.scrollbox = new ScrollBox(this);
   this.scrollbox.autoScroll = true;
   this.scrollbox.tracking = true;
   this.scrollbox.cursor = new Cursor(StdCursor_Arrow);

   this.scrollbox.onHorizontalScrollPosUpdated = function(newPos)
   {
      this.viewport.update();
   };

   this.scrollbox.onVerticalScrollPosUpdated = function(newPos)
   {
      this.viewport.update();
   };

   // Viewport paint handler - uses VectorGraphics per official pattern
   this.scrollbox.viewport.onPaint = function(x0, y0, x1, y1)
   {
      var graphics = new VectorGraphics(this);

      // Fill background
      graphics.fillRect(x0, y0, x1, y1, new Brush(0xff202020));

      if (self.scaledImage)
      {
         // Calculate offset to center image or handle scrolling
         var offsetX = (this.parent.maxHorizontalScrollPosition > 0) ?
            -this.parent.horizontalScrollPosition :
            (this.width - self.scaledImage.width) / 2;
         var offsetY = (this.parent.maxVerticalScrollPosition > 0) ?
            -this.parent.verticalScrollPosition :
            (this.height - self.scaledImage.height) / 2;

         graphics.translateTransformation(offsetX, offsetY);

         // Draw the preview image
         graphics.drawBitmap(0, 0, self.scaledImage);

         // Draw border
         graphics.pen = new Pen(0xffffffff, 0);
         graphics.drawRect(-1, -1, self.scaledImage.width + 1, self.scaledImage.height + 1);

         // Draw split line if in split mode
         if (self.previewMode === 2)
         {
            var splitX = Math.round(self.scaledImage.width * self.splitPosition / 100);
            graphics.pen = new Pen(0xaaffffff, 2);
            graphics.drawLine(splitX, 0, splitX, self.scaledImage.height);
         }

         // Draw mode labels
         graphics.antialiasing = true;
         graphics.pen = new Pen(0xffffffff);
         if (self.previewMode === 2)
         {
            graphics.drawText(5, 15, "BEFORE");
            graphics.drawText(self.scaledImage.width - 45, 15, "AFTER");
         }
         else if (self.previewMode === 1)
         {
            graphics.drawText(5, 15, "BEFORE (Linear)");
         }
         else
         {
            graphics.drawText(5, 15, "AFTER (Lupton RGB)");
         }
      }
      else
      {
         // No image loaded
         graphics.pen = new Pen(0xff888888);
         graphics.drawText(this.width / 2 - 50, this.height / 2, "No image loaded");
      }

      graphics.end();
   };

   // Mouse tracking on viewport
   this.scrollbox.viewport.onMouseMove = function(x, y, buttonState, modifiers)
   {
      if (self.scrolling)
      {
         self.scrollbox.horizontalScrollPosition = self.scrolling.orgScroll.x - (x - self.scrolling.orgCursor.x);
         self.scrollbox.verticalScrollPosition = self.scrolling.orgScroll.y - (y - self.scrolling.orgCursor.y);
      }

      // Update cursor info callback
      if (self.onCursorCallback && self.sourceWindow && self.scaledImage)
      {
         var ox = (this.parent.maxHorizontalScrollPosition > 0) ?
            -this.parent.horizontalScrollPosition :
            (this.width - self.scaledImage.width) / 2;
         var oy = (this.parent.maxVerticalScrollPosition > 0) ?
            -this.parent.verticalScrollPosition :
            (this.height - self.scaledImage.height) / 2;

         var px = (x - ox) / self.scale;
         var py = (y - oy) / self.scale;

         var image = self.sourceWindow.mainView.image;
         if (px >= 0 && px < image.width && py >= 0 && py < image.height)
         {
            var ix = Math.floor(px);
            var iy = Math.floor(py);
            var r = image.sample(ix, iy, 0);
            var g = image.sample(ix, iy, 1);
            var b = image.sample(ix, iy, 2);
            self.onCursorCallback(ix, iy, r, g, b);
         }
      }
   };

   // Mouse press for pan and sampling
   this.scrollbox.viewport.onMousePress = function(x, y, button, buttonState, modifiers)
   {
      // Handle sampling mode
      if (self.samplingMode && self.sourceWindow && self.scaledImage)
      {
         var ox = (this.parent.maxHorizontalScrollPosition > 0) ?
            -this.parent.horizontalScrollPosition :
            (this.width - self.scaledImage.width) / 2;
         var oy = (this.parent.maxVerticalScrollPosition > 0) ?
            -this.parent.verticalScrollPosition :
            (this.height - self.scaledImage.height) / 2;

         var px = (x - ox) / self.scale;
         var py = (y - oy) / self.scale;

         var image = self.sourceWindow.mainView.image;
         if (px >= 0 && px < image.width && py >= 0 && py < image.height)
         {
            var ix = Math.floor(px);
            var iy = Math.floor(py);
            var r = image.sample(ix, iy, 0);
            var g = image.sample(ix, iy, 1);
            var b = image.sample(ix, iy, 2);

            if (self.onSampleCallback)
               self.onSampleCallback(r, g, b);
         }

         self.samplingMode = false;
         this.cursor = new Cursor(StdCursor_Arrow);
         return;
      }

      // Handle pan
      if (self.scrolling || button != MouseButton_Left)
         return;

      self.scrolling = {
         orgCursor: new Point(x, y),
         orgScroll: new Point(self.scrollbox.horizontalScrollPosition, self.scrollbox.verticalScrollPosition)
      };
      this.cursor = new Cursor(StdCursor_ClosedHand);
   };

   this.scrollbox.viewport.onMouseRelease = function(x, y, button, buttonState, modifiers)
   {
      if (self.scrolling)
      {
         self.scrolling = null;
         this.cursor = new Cursor(StdCursor_Arrow);
      }
   };

   // Mouse wheel for zoom
   this.scrollbox.viewport.onMouseWheel = function(x, y, delta, buttonState, modifiers)
   {
      self.UpdateZoom(self.zoom + (delta > 0 ? -1 : 1), new Point(x, y));
   };

   // Resize handler
   this.scrollbox.viewport.onResize = function(wNew, hNew, wOld, hOld)
   {
      self.SetZoomOutLimit();
      if (self.zoom < self.zoomOutLimit)
         self.UpdateZoom(self.zoomOutLimit);
      else
         self.forceRedraw();
   };

   // Layout
   this.sizer = new VerticalSizer;
   this.sizer.add(this.scrollbox);

   this.setScaledMinSize(320, 240);

   // Sampling mode
   this.samplingMode = false;
   this.onSampleCallback = null;
   this.onCursorCallback = null;
   this.scrolling = null;

   // Calculate zoom out limit based on viewport size
   this.SetZoomOutLimit = function()
   {
      if (!this.sourceWindow) return;
      var image = this.sourceWindow.mainView.image;
      var scaleX = Math.ceil(image.width / this.scrollbox.viewport.width);
      var scaleY = Math.ceil(image.height / this.scrollbox.viewport.height);
      var scale = Math.max(scaleX, scaleY);
      this.zoomOutLimit = Math.min(0, -scale + 2);
   };

   // Update zoom level
   this.UpdateZoom = function(newZoom, refPoint)
   {
      newZoom = Math.max(this.zoomOutLimit, Math.min(2, newZoom));
      if (newZoom == this.zoom && this.scaledImage)
         return;

      if (!refPoint)
         refPoint = new Point(this.scrollbox.viewport.width / 2, this.scrollbox.viewport.height / 2);

      var imgx = null;
      if (this.scrollbox.maxHorizontalScrollPosition > 0)
         imgx = (refPoint.x + this.scrollbox.horizontalScrollPosition) / this.scale;
      var imgy = null;
      if (this.scrollbox.maxVerticalScrollPosition > 0)
         imgy = (refPoint.y + this.scrollbox.verticalScrollPosition) / this.scale;

      this.zoom = newZoom;
      this.zoomLevel = newZoom;  // For external compatibility

      if (this.zoom > 0)
         this.scale = this.zoom;
      else
         this.scale = 1 / (-this.zoom + 2);

      this.zoomFactor = this.scale;  // For external compatibility

      this.regenerateScaledImage();

      if (this.scaledImage)
      {
         this.scrollbox.maxHorizontalScrollPosition = Math.max(0, this.scaledImage.width - this.scrollbox.viewport.width);
         this.scrollbox.maxVerticalScrollPosition = Math.max(0, this.scaledImage.height - this.scrollbox.viewport.height);

         if (this.scrollbox.maxHorizontalScrollPosition > 0 && imgx != null)
            this.scrollbox.horizontalScrollPosition = (imgx * this.scale) - refPoint.x;
         if (this.scrollbox.maxVerticalScrollPosition > 0 && imgy != null)
            this.scrollbox.verticalScrollPosition = (imgy * this.scale) - refPoint.y;
      }

      this.scrollbox.viewport.update();
   };

   // Regenerate the scaled preview image
   this.regenerateScaledImage = function()
   {
      this.scaledImage = null;
      if (!this.sourceWindow) return;

      var image = this.sourceWindow.mainView.image;

      // Calculate the output size based on current scale
      var outWidth = Math.round(image.width * this.scale);
      var outHeight = Math.round(image.height * this.scale);

      // Generate preview at the exact size we need
      var showBefore = this.previewMode;
      this.scaledImage = this.engine.generatePreviewAtSize(
         this.sourceWindow,
         outWidth,
         outHeight,
         showBefore,
         this.splitPosition
      );
   };

   // Force redraw - official PJSR pattern
   this.forceRedraw = function()
   {
      this.scrollbox.viewport.update();
   };

   // Update the preview (regenerate and redraw)
   this.updatePreview = function()
   {
      if (!this.sourceWindow)
      {
         this.scaledImage = null;
         this.forceRedraw();
         return;
      }

      if (this.scrollbox.viewport.width <= 0 || this.scrollbox.viewport.height <= 0)
         return;

      this.SetZoomOutLimit();
      this.regenerateScaledImage();

      if (this.scaledImage)
      {
         this.scrollbox.maxHorizontalScrollPosition = Math.max(0, this.scaledImage.width - this.scrollbox.viewport.width);
         this.scrollbox.maxVerticalScrollPosition = Math.max(0, this.scaledImage.height - this.scrollbox.viewport.height);
      }

      this.forceRedraw();
   };

   // Zoom in
   this.zoomIn = function()
   {
      this.UpdateZoom(this.zoom + 1);
   };

   // Zoom out
   this.zoomOut = function()
   {
      this.UpdateZoom(this.zoom - 1);
   };

   // Fit to window - calculate zoom that fits image in viewport
   this.fitToWindow = function()
   {
      if (!this.sourceWindow) return;
      var image = this.sourceWindow.mainView.image;
      var vpWidth = this.scrollbox.viewport.width;
      var vpHeight = this.scrollbox.viewport.height;
      if (vpWidth <= 0 || vpHeight <= 0) return;

      // Calculate scale needed to fit
      var scaleX = vpWidth / image.width;
      var scaleY = vpHeight / image.height;
      var fitScale = Math.min(scaleX, scaleY);

      // Convert scale to zoom level
      // zoom > 0: scale = zoom (so zoom = scale)
      // zoom <= 0: scale = 1 / (-zoom + 2), so zoom = 2 - (1/scale)
      var fitZoom;
      if (fitScale >= 1)
         fitZoom = Math.floor(fitScale);
      else
         fitZoom = Math.floor(2 - (1 / fitScale));

      fitZoom = Math.max(this.zoomOutLimit, Math.min(2, fitZoom));
      this.UpdateZoom(fitZoom);
   };

   // Get zoom label text
   this.getZoomText = function()
   {
      if (this.zoom === 0) return "Fit";
      if (this.zoom > 0) return this.zoom + ":1";
      return "1:" + (-this.zoom + 2);
   };
}

PreviewControl.prototype = new Frame;

// ============================================================================
// Main Dialog
// ============================================================================

function LuptonDialog(engine)
{
   this.__base__ = Dialog;
   this.__base__();

   this.engine = engine;
   this.targetWindow = null;

   this.windowTitle = TITLE + " v" + VERSION;
   this.minWidth = 850;
   this.minHeight = 550;

   // -------------------------------------------------------------------------
   // Left Panel - Controls
   // -------------------------------------------------------------------------

   // --- Input Images Group ---
   this.targetImageLabel = new Label(this);
   this.targetImageLabel.text = "Target:";
   this.targetImageLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.targetImageLabel.setFixedWidth(45);

   this.targetImageCombo = new ComboBox(this);
   this.targetImageCombo.toolTip = "Select target RGB image to process";
   this.targetImageCombo.onItemSelected = function(index)
   {
      if (index > 0)
      {
         var windows = ImageWindow.windows;
         if (index - 1 < windows.length)
         {
            this.dialog.targetWindow = windows[index - 1];
            this.dialog.previewControl.sourceWindow = this.dialog.targetWindow;
            this.dialog.updateTargetWindow();
         }
      }
   };

   var targetSizer = new HorizontalSizer;
   targetSizer.spacing = 4;
   targetSizer.add(this.targetImageLabel);
   targetSizer.add(this.targetImageCombo, 100);

   this.useActiveCheckbox = new CheckBox(this);
   this.useActiveCheckbox.text = "Use active RGB image";
   this.useActiveCheckbox.checked = this.engine.useActiveImage;
   this.useActiveCheckbox.toolTip = "Use the currently active RGB image as input";
   this.useActiveCheckbox.onCheck = function(checked)
   {
      this.dialog.engine.useActiveImage = checked;
      this.dialog.targetImageCombo.enabled = !checked;
      this.dialog.imageRCombo.enabled = !checked;
      this.dialog.imageGCombo.enabled = !checked;
      this.dialog.imageBCombo.enabled = !checked;
      this.dialog.updateTargetWindow();
   };

   // Image combo boxes
   this.imageRLabel = new Label(this);
   this.imageRLabel.text = "R:";
   this.imageRLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.imageRLabel.setFixedWidth(20);

   this.imageRCombo = new ComboBox(this);
   this.imageRCombo.enabled = !this.engine.useActiveImage;
   this.imageRCombo.toolTip = "Select image for red channel";

   this.imageGLabel = new Label(this);
   this.imageGLabel.text = "G:";
   this.imageGLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.imageGLabel.setFixedWidth(20);

   this.imageGCombo = new ComboBox(this);
   this.imageGCombo.enabled = !this.engine.useActiveImage;
   this.imageGCombo.toolTip = "Select image for green channel";

   this.imageBLabel = new Label(this);
   this.imageBLabel.text = "B:";
   this.imageBLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.imageBLabel.setFixedWidth(20);

   this.imageBCombo = new ComboBox(this);
   this.imageBCombo.enabled = !this.engine.useActiveImage;
   this.imageBCombo.toolTip = "Select image for blue channel";

   // Populate image lists
   this.populateImageLists = function()
   {
      var windows = ImageWindow.windows;
      this.targetImageCombo.clear();
      this.imageRCombo.clear();
      this.imageGCombo.clear();
      this.imageBCombo.clear();

      this.targetImageCombo.addItem("<select>");
      this.imageRCombo.addItem("<select>");
      this.imageGCombo.addItem("<select>");
      this.imageBCombo.addItem("<select>");

      for (var i = 0; i < windows.length; i++)
      {
         var id = windows[i].mainView.id;
         this.targetImageCombo.addItem(id);
         this.imageRCombo.addItem(id);
         this.imageGCombo.addItem(id);
         this.imageBCombo.addItem(id);
      }

      // Select current target window in combo if available
      if (this.targetWindow)
      {
         for (var i = 0; i < windows.length; i++)
         {
            if (windows[i].mainView.id === this.targetWindow.mainView.id)
            {
               this.targetImageCombo.currentItem = i + 1;
               break;
            }
         }
      }
   };

   var rSizer = new HorizontalSizer;
   rSizer.spacing = 4;
   rSizer.add(this.imageRLabel);
   rSizer.add(this.imageRCombo, 100);

   var gSizer = new HorizontalSizer;
   gSizer.spacing = 4;
   gSizer.add(this.imageGLabel);
   gSizer.add(this.imageGCombo, 100);

   var bSizer = new HorizontalSizer;
   bSizer.spacing = 4;
   bSizer.add(this.imageBLabel);
   bSizer.add(this.imageBCombo, 100);

   // Set initial enabled state for target combo
   this.targetImageCombo.enabled = !this.engine.useActiveImage;

   this.inputGroup = new GroupBox(this);
   this.inputGroup.title = "Input Images";
   this.inputGroup.sizer = new VerticalSizer;
   this.inputGroup.sizer.margin = 6;
   this.inputGroup.sizer.spacing = 4;
   this.inputGroup.sizer.add(targetSizer);
   this.inputGroup.sizer.add(this.useActiveCheckbox);
   this.inputGroup.sizer.add(rSizer);
   this.inputGroup.sizer.add(gSizer);
   this.inputGroup.sizer.add(bSizer);

   // --- Stretch Parameters Group ---
   this.stretchControl = new NumericControl(this);
   this.stretchControl.label.text = "Stretch (\u03B1):";
   this.stretchControl.label.setFixedWidth(80);
   this.stretchControl.setRange(0.1, 1000.0);
   this.stretchControl.slider.setRange(0, 10000);
   this.stretchControl.slider.minWidth = 150;
   this.stretchControl.setPrecision(2);
   this.stretchControl.setValue(this.engine.stretch);
   this.stretchControl.toolTip = "Linear amplification factor. Higher = brighter image";
   this.stretchControl.onValueUpdated = function(value)
   {
      this.dialog.engine.stretch = value;
      this.dialog.schedulePreviewUpdate();
   };

   this.qControl = new NumericControl(this);
   this.qControl.label.text = "Q (softening):";
   this.qControl.label.setFixedWidth(80);
   this.qControl.setRange(-10.0, 30.0);
   this.qControl.slider.setRange(0, 4000);
   this.qControl.slider.minWidth = 150;
   this.qControl.setPrecision(2);
   this.qControl.setValue(this.engine.Q);
   this.qControl.toolTip = "Controls linear-to-logarithmic transition. Lower Q = earlier log behavior";
   this.qControl.onValueUpdated = function(value)
   {
      this.dialog.engine.Q = value;
      this.dialog.schedulePreviewUpdate();
   };

   this.qHelpLabel = new Label(this);
   this.qHelpLabel.text = "Lower Q = earlier log transition";
   this.qHelpLabel.textAlignment = TextAlign_Left;

   this.stretchGroup = new GroupBox(this);
   this.stretchGroup.title = "Stretch Parameters";
   this.stretchGroup.sizer = new VerticalSizer;
   this.stretchGroup.sizer.margin = 6;
   this.stretchGroup.sizer.spacing = 4;
   this.stretchGroup.sizer.add(this.stretchControl);
   this.stretchGroup.sizer.add(this.qControl);
   this.stretchGroup.sizer.add(this.qHelpLabel);

   // --- Black Point Group ---
   this.linkedCheckbox = new CheckBox(this);
   this.linkedCheckbox.text = "Link RGB channels";
   this.linkedCheckbox.checked = this.engine.linkedChannels;
   this.linkedCheckbox.toolTip = "Use the same black point for all channels";
   this.linkedCheckbox.onCheck = function(checked)
   {
      this.dialog.engine.linkedChannels = checked;
      this.dialog.blackPointControl.visible = checked;
      this.dialog.blackRControl.visible = !checked;
      this.dialog.blackGControl.visible = !checked;
      this.dialog.blackBControl.visible = !checked;
      this.dialog.adjustToContents();
      this.dialog.schedulePreviewUpdate();
   };

   // Black point scale: 0-100 displayed, actual = displayed / 10000
   // So 20 displayed = 0.0020 actual
   this.blackPointControl = new NumericControl(this);
   this.blackPointControl.label.text = "Black Point:";
   this.blackPointControl.label.setFixedWidth(80);
   this.blackPointControl.setRange(0, 100);
   this.blackPointControl.slider.setRange(0, 1000);
   this.blackPointControl.slider.minWidth = 150;
   this.blackPointControl.setPrecision(0);
   this.blackPointControl.setValue(this.engine.blackPoint * 10000);
   this.blackPointControl.toolTip = "Value subtracted before stretch (0-100 scale, actual = value/10000)";
   this.blackPointControl.onValueUpdated = function(value)
   {
      this.dialog.engine.blackPoint = value / 10000;
      this.dialog.schedulePreviewUpdate();
   };

   this.blackRControl = new NumericControl(this);
   this.blackRControl.label.text = "Black (R):";
   this.blackRControl.label.setFixedWidth(80);
   this.blackRControl.setRange(0, 100);
   this.blackRControl.slider.setRange(0, 1000);
   this.blackRControl.slider.minWidth = 150;
   this.blackRControl.setPrecision(0);
   this.blackRControl.setValue(this.engine.blackR * 10000);
   this.blackRControl.visible = false;
   this.blackRControl.onValueUpdated = function(value)
   {
      this.dialog.engine.blackR = value / 10000;
      this.dialog.schedulePreviewUpdate();
   };

   this.blackGControl = new NumericControl(this);
   this.blackGControl.label.text = "Black (G):";
   this.blackGControl.label.setFixedWidth(80);
   this.blackGControl.setRange(0, 100);
   this.blackGControl.slider.setRange(0, 1000);
   this.blackGControl.slider.minWidth = 150;
   this.blackGControl.setPrecision(0);
   this.blackGControl.setValue(this.engine.blackG * 10000);
   this.blackGControl.visible = false;
   this.blackGControl.onValueUpdated = function(value)
   {
      this.dialog.engine.blackG = value / 10000;
      this.dialog.schedulePreviewUpdate();
   };

   this.blackBControl = new NumericControl(this);
   this.blackBControl.label.text = "Black (B):";
   this.blackBControl.label.setFixedWidth(80);
   this.blackBControl.setRange(0, 100);
   this.blackBControl.slider.setRange(0, 1000);
   this.blackBControl.slider.minWidth = 150;
   this.blackBControl.setPrecision(0);
   this.blackBControl.setValue(this.engine.blackB * 10000);
   this.blackBControl.visible = false;
   this.blackBControl.onValueUpdated = function(value)
   {
      this.dialog.engine.blackB = value / 10000;
      this.dialog.schedulePreviewUpdate();
   };

   this.autoBlackButton = new PushButton(this);
   this.autoBlackButton.text = "Auto";
   this.autoBlackButton.setFixedWidth(50);
   this.autoBlackButton.toolTip = "Calculate black point from image statistics";
   this.autoBlackButton.onClick = function()
   {
      this.dialog.calculateAutoBlackPoint();
   };

   this.sampleBlackButton = new PushButton(this);
   this.sampleBlackButton.text = "Sample";
   this.sampleBlackButton.setFixedWidth(50);
   this.sampleBlackButton.toolTip = "Sample background from preview (click on dark area)";
   this.sampleBlackButton.onClick = function()
   {
      // Enable sampling mode on the preview
      this.dialog.previewControl.samplingMode = true;
      this.dialog.previewControl.cursor = new Cursor(StdCursor_Cross);
      this.dialog.statusLabel.text = "Click on a dark background area in the preview...";
   };

   var blackButtonsSizer = new HorizontalSizer;
   blackButtonsSizer.spacing = 4;
   blackButtonsSizer.addSpacing(85);
   blackButtonsSizer.add(this.autoBlackButton);
   blackButtonsSizer.add(this.sampleBlackButton);
   blackButtonsSizer.addStretch();

   this.blackPointGroup = new GroupBox(this);
   this.blackPointGroup.title = "Black Point";
   this.blackPointGroup.sizer = new VerticalSizer;
   this.blackPointGroup.sizer.margin = 6;
   this.blackPointGroup.sizer.spacing = 4;
   this.blackPointGroup.sizer.add(this.linkedCheckbox);
   this.blackPointGroup.sizer.add(this.blackPointControl);
   this.blackPointGroup.sizer.add(this.blackRControl);
   this.blackPointGroup.sizer.add(this.blackGControl);
   this.blackPointGroup.sizer.add(this.blackBControl);
   this.blackPointGroup.sizer.add(blackButtonsSizer);

   // --- Color Options Group ---
   this.saturationControl = new NumericControl(this);
   this.saturationControl.label.text = "Saturation:";
   this.saturationControl.label.setFixedWidth(80);
   this.saturationControl.setRange(0.5, 2.0);
   this.saturationControl.slider.setRange(0, 150);
   this.saturationControl.slider.minWidth = 150;
   this.saturationControl.setPrecision(2);
   this.saturationControl.setValue(this.engine.saturation);
   this.saturationControl.toolTip = "Post-stretch saturation adjustment";
   this.saturationControl.onValueUpdated = function(value)
   {
      this.dialog.engine.saturation = value;
      this.dialog.schedulePreviewUpdate();
   };

   this.clippingLabel = new Label(this);
   this.clippingLabel.text = "Clipping:";
   this.clippingLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.clippingLabel.setFixedWidth(80);

   this.clippingCombo = new ComboBox(this);
   this.clippingCombo.addItem("Preserve Color (Lupton)");
   this.clippingCombo.addItem("Hard Clip");
   this.clippingCombo.addItem("Rescale to Max");
   this.clippingCombo.currentItem = this.engine.clippingMode;
   this.clippingCombo.toolTip = "How to handle values exceeding 1.0";
   this.clippingCombo.onItemSelected = function(index)
   {
      this.dialog.engine.clippingMode = index;
      this.dialog.schedulePreviewUpdate();
   };

   var clippingSizer = new HorizontalSizer;
   clippingSizer.spacing = 4;
   clippingSizer.add(this.clippingLabel);
   clippingSizer.add(this.clippingCombo, 100);

   this.colorGroup = new GroupBox(this);
   this.colorGroup.title = "Color Options";
   this.colorGroup.sizer = new VerticalSizer;
   this.colorGroup.sizer.margin = 6;
   this.colorGroup.sizer.spacing = 4;
   this.colorGroup.sizer.add(this.saturationControl);
   this.colorGroup.sizer.add(clippingSizer);

   // --- Action Buttons ---

   this.resetButton = new PushButton(this);
   this.resetButton.text = "Reset";
   this.resetButton.toolTip = "Reset all parameters to default values";
   this.resetButton.onClick = function()
   {
      this.dialog.engine.reset();
      this.dialog.updateControlsFromEngine();
      this.dialog.schedulePreviewUpdate();
   };

   this.applyButton = new PushButton(this);
   this.applyButton.text = "Execute";
   this.applyButton.toolTip = "Execute Lupton RGB stretch to create new image";
   this.applyButton.onClick = function()
   {
      this.dialog.apply();
   };

   this.closeButton = new PushButton(this);
   this.closeButton.text = "Close";
   this.closeButton.toolTip = "Close dialog";
   this.closeButton.onClick = function()
   {
      this.dialog.cancel();
   };

   var actionSizer = new HorizontalSizer;
   actionSizer.spacing = 6;
   actionSizer.addStretch();
   actionSizer.add(this.resetButton);
   actionSizer.add(this.applyButton);
   actionSizer.add(this.closeButton);

   // --- Preview Options Group ---
   this.showPreviewCheckbox = new CheckBox(this);
   this.showPreviewCheckbox.text = "Show Preview";
   this.showPreviewCheckbox.checked = true;
   this.showPreviewCheckbox.toolTip = "Show/hide the preview panel";
   // Note: onCheck handler is set after dlg is defined in the right panel section

   this.previewOptionsGroup = new GroupBox(this);
   this.previewOptionsGroup.title = "Preview Options";
   this.previewOptionsGroup.sizer = new VerticalSizer;
   this.previewOptionsGroup.sizer.margin = 6;
   this.previewOptionsGroup.sizer.spacing = 4;
   this.previewOptionsGroup.sizer.add(this.showPreviewCheckbox);

   // --- Left Panel Assembly ---
   this.leftPanel = new Control(this);
   this.leftPanel.setFixedWidth(310);
   this.leftPanel.sizer = new VerticalSizer;
   this.leftPanel.sizer.margin = 6;
   this.leftPanel.sizer.spacing = 8;
   this.leftPanel.sizer.add(this.inputGroup);
   this.leftPanel.sizer.add(this.stretchGroup);
   this.leftPanel.sizer.add(this.blackPointGroup);
   this.leftPanel.sizer.add(this.colorGroup);
   this.leftPanel.sizer.add(this.previewOptionsGroup);
   this.leftPanel.sizer.addStretch();
   this.leftPanel.sizer.add(actionSizer);

   // -------------------------------------------------------------------------
   // Right Panel - Preview
   // -------------------------------------------------------------------------

   // Capture dialog reference for use in callbacks
   var dlg = this;

   // Set up show/hide preview checkbox handler (needs dlg reference)
   this.showPreviewCheckbox.onCheck = function(checked)
   {
      dlg.rightPanel.visible = checked;
      if (checked)
         dlg.forcePreviewUpdate();
      dlg.adjustToContents();
   };

   // Preview toolbar
   this.beforeButton = new PushButton(this);
   this.beforeButton.text = "Before";
   this.beforeButton.setFixedWidth(50);
   this.beforeButton.toolTip = "Show original image";
   this.beforeButton.onClick = function()
   {
      dlg.previewControl.previewMode = 1;
      dlg.updatePreviewModeButtons();
      dlg.schedulePreviewUpdate();
   };

   this.splitButton = new PushButton(this);
   this.splitButton.text = "Split";
   this.splitButton.setFixedWidth(50);
   this.splitButton.toolTip = "Show split before/after view";
   this.splitButton.onClick = function()
   {
      dlg.previewControl.previewMode = 2;
      dlg.updatePreviewModeButtons();
      dlg.schedulePreviewUpdate();
   };

   this.afterButton = new PushButton(this);
   this.afterButton.text = "After";
   this.afterButton.setFixedWidth(50);
   this.afterButton.toolTip = "Show processed image";
   this.afterButton.onClick = function()
   {
      dlg.previewControl.previewMode = 0;
      dlg.updatePreviewModeButtons();
      dlg.schedulePreviewUpdate();
   };

   // Zoom controls
   this.zoomOutButton = new ToolButton(this);
   this.zoomOutButton.text = "-";
   this.zoomOutButton.setFixedWidth(24);
   this.zoomOutButton.toolTip = "Zoom out";
   this.zoomOutButton.onClick = function()
   {
      dlg.previewControl.zoomOut();
      dlg.updateZoomLabel();
   };

   this.zoomLabel = new Label(this);
   this.zoomLabel.text = "Fit";
   this.zoomLabel.textAlignment = TextAlign_Center;
   this.zoomLabel.setFixedWidth(45);

   this.zoomInButton = new ToolButton(this);
   this.zoomInButton.text = "+";
   this.zoomInButton.setFixedWidth(24);
   this.zoomInButton.toolTip = "Zoom in";
   this.zoomInButton.onClick = function()
   {
      dlg.previewControl.zoomIn();
      dlg.updateZoomLabel();
   };

   this.fitButton = new PushButton(this);
   this.fitButton.text = "Fit";
   this.fitButton.setFixedWidth(35);
   this.fitButton.toolTip = "Fit image to preview window";
   this.fitButton.onClick = function()
   {
      dlg.previewControl.fitToWindow();
      dlg.updateZoomLabel();
   };

   var previewToolbar = new HorizontalSizer;
   previewToolbar.spacing = 6;
   previewToolbar.add(this.beforeButton);
   previewToolbar.add(this.splitButton);
   previewToolbar.add(this.afterButton);
   previewToolbar.addSpacing(10);
   previewToolbar.add(this.zoomOutButton);
   previewToolbar.add(this.zoomLabel);
   previewToolbar.add(this.zoomInButton);
   previewToolbar.add(this.fitButton);

   // Preview canvas
   this.previewControl = new PreviewControl(this, this.engine);
   this.previewControl.setMinSize(400, 300);

   // Set up sampling callback
   // Sampled values are actual pixel values, controls display value * 10000
   this.previewControl.onSampleCallback = function(r, g, b)
   {
      if (dlg.engine.linkedChannels)
      {
         // Use average as black point
         var avg = (r + g + b) / 3;
         dlg.engine.blackPoint = avg;
         dlg.blackPointControl.setValue(avg * 10000);  // Convert to display scale
         console.writeln(format("Sampled black point: %.6f (display: %.1f)", avg, avg * 10000));
      }
      else
      {
         dlg.engine.blackR = r;
         dlg.engine.blackG = g;
         dlg.engine.blackB = b;
         dlg.blackRControl.setValue(r * 10000);  // Convert to display scale
         dlg.blackGControl.setValue(g * 10000);
         dlg.blackBControl.setValue(b * 10000);
         console.writeln(format("Sampled black point R: %.6f, G: %.6f, B: %.6f", r, g, b));
      }
      dlg.statusLabel.text = "Black point sampled from preview";
      dlg.schedulePreviewUpdate();
   };

   // Set up cursor tracking callback
   this.previewControl.onCursorCallback = function(ix, iy, r, g, b)
   {
      dlg.cursorInfoLabel.text = format("Cursor: (%d, %d) | R=%.4f G=%.4f B=%.4f", ix, iy, r, g, b);
   };

   // Split position slider
   this.splitLabel = new Label(this);
   this.splitLabel.text = "Split:";
   this.splitLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   this.splitLabel.setFixedWidth(35);

   this.splitSlider = new HorizontalSlider(this);
   this.splitSlider.setRange(10, 90);
   this.splitSlider.value = 50;
   this.splitSlider.toolTip = "Adjust split position";
   this.splitSlider.onValueUpdated = function(value)
   {
      dlg.previewControl.splitPosition = value;
      dlg.schedulePreviewUpdate();
   };

   this.splitControl = new Control(this);
   this.splitControl.sizer = new HorizontalSizer;
   this.splitControl.sizer.spacing = 4;
   this.splitControl.sizer.add(this.splitLabel);
   this.splitControl.sizer.add(this.splitSlider, 100);

   // Cursor info bar
   this.cursorInfoLabel = new Label(this);
   this.cursorInfoLabel.text = "Cursor: (---, ---) | R=-.--- G=-.--- B=-.---";
   this.cursorInfoLabel.textAlignment = TextAlign_Left;

   this.imageSizeLabel = new Label(this);
   this.imageSizeLabel.text = "--- x --- px | 32-bit";
   this.imageSizeLabel.textAlignment = TextAlign_Right;

   var infoSizer = new HorizontalSizer;
   infoSizer.add(this.cursorInfoLabel);
   infoSizer.addStretch();
   infoSizer.add(this.imageSizeLabel);

   // Right panel assembly
   this.rightPanel = new Control(this);
   this.rightPanel.sizer = new VerticalSizer;
   this.rightPanel.sizer.margin = 6;
   this.rightPanel.sizer.spacing = 6;
   this.rightPanel.sizer.add(previewToolbar);
   this.rightPanel.sizer.add(this.previewControl, 100);
   this.rightPanel.sizer.add(this.splitControl);
   this.rightPanel.sizer.add(infoSizer);

   // -------------------------------------------------------------------------
   // Main Layout
   // -------------------------------------------------------------------------

   var mainSizer = new HorizontalSizer;
   mainSizer.add(this.leftPanel);
   mainSizer.add(this.rightPanel, 100);

   // Status bar
   this.statusLabel = new Label(this);
   this.statusLabel.text = "Lupton RGB v" + VERSION + " | Based on Lupton et al. (2004) PASP 116:133";
   this.statusLabel.textAlignment = TextAlign_Left;

   this.timeLabel = new Label(this);
   this.timeLabel.text = "";
   this.timeLabel.textAlignment = TextAlign_Right;

   var statusSizer = new HorizontalSizer;
   statusSizer.margin = 4;
   statusSizer.add(this.statusLabel);
   statusSizer.addStretch();
   statusSizer.add(this.timeLabel);

   this.sizer = new VerticalSizer;
   this.sizer.add(mainSizer, 100);
   this.sizer.add(statusSizer);

   // -------------------------------------------------------------------------
   // Helper Methods
   // -------------------------------------------------------------------------

   this.updatePreviewModeButtons = function()
   {
      var mode = this.previewControl.previewMode;
      // Update button text to show which is active
      this.beforeButton.text = (mode === 1) ? "[Before]" : "Before";
      this.splitButton.text = (mode === 2) ? "[Split]" : "Split";
      this.afterButton.text = (mode === 0) ? "[After]" : "After";
      this.splitControl.visible = (mode === 2);
   };

   this.updateZoomLabel = function()
   {
      this.zoomLabel.text = this.previewControl.getZoomText();
   };

   this.updateTargetWindow = function()
   {
      if (this.engine.useActiveImage)
      {
         this.targetWindow = ImageWindow.activeWindow;
      }
      // else: keep the targetWindow that was set by the dropdown

      this.previewControl.sourceWindow = this.targetWindow;

      if (this.targetWindow && this.targetWindow.mainView && this.targetWindow.mainView.image)
      {
         var img = this.targetWindow.mainView.image;
         this.imageSizeLabel.text = format("%d x %d px | 32-bit", img.width, img.height);
      }
      else
      {
         this.imageSizeLabel.text = "No image";
      }

      this.schedulePreviewUpdate();
   };

   // Throttling state
   this.lastPreviewTime = 0;
   this.previewSkipCount = 0;

   this.schedulePreviewUpdate = function()
   {
      if (!this.showPreviewCheckbox.checked)
         return;

      // Throttle updates - skip if last update was less than 80ms ago
      var now = new Date().getTime();
      var elapsed = now - this.lastPreviewTime;

      if (elapsed < 80)
      {
         // Skip this update but count it
         this.previewSkipCount++;
         // Only force update every 4th skip to keep preview somewhat responsive
         if (this.previewSkipCount < 4)
            return;
      }

      this.previewSkipCount = 0;
      this.lastPreviewTime = now;

      var start = now;
      this.previewControl.updatePreview();
      var renderTime = (new Date().getTime() - start) / 1000;
      this.timeLabel.text = format("Preview: %.2fs", renderTime);
   };

   // Force preview update (bypasses throttling) - use for button clicks
   this.forcePreviewUpdate = function()
   {
      this.lastPreviewTime = 0;  // Reset throttle
      this.previewSkipCount = 0;
      var start = new Date().getTime();
      this.previewControl.updatePreview();
      var renderTime = (new Date().getTime() - start) / 1000;
      this.timeLabel.text = format("Preview: %.2fs", renderTime);
   };

   this.calculateAutoBlackPoint = function()
   {
      if (!this.targetWindow)
      {
         console.warningln("No image selected for auto black point calculation");
         return;
      }

      var view = this.targetWindow.mainView;

      if (this.engine.linkedChannels)
      {
         // Calculate average black point across channels
         var bp0 = this.engine.calculateAutoBlackPoint(view, 0);
         var bp1 = this.engine.calculateAutoBlackPoint(view, 1);
         var bp2 = this.engine.calculateAutoBlackPoint(view, 2);
         var avgBp = (bp0 + bp1 + bp2) / 3;

         this.engine.blackPoint = avgBp;
         this.blackPointControl.setValue(avgBp * 10000);  // Convert to display scale
         console.writeln(format("Auto black point (linked): %.6f (display: %.1f)", avgBp, avgBp * 10000));
      }
      else
      {
         var bpR = this.engine.calculateAutoBlackPoint(view, 0);
         var bpG = this.engine.calculateAutoBlackPoint(view, 1);
         var bpB = this.engine.calculateAutoBlackPoint(view, 2);

         this.engine.blackR = bpR;
         this.engine.blackG = bpG;
         this.engine.blackB = bpB;

         this.blackRControl.setValue(bpR * 10000);  // Convert to display scale
         this.blackGControl.setValue(bpG * 10000);
         this.blackBControl.setValue(bpB * 10000);

         console.writeln(format("Auto black point R: %.6f, G: %.6f, B: %.6f", bpR, bpG, bpB));
      }

      this.schedulePreviewUpdate();
   };

   this.updateControlsFromEngine = function()
   {
      this.stretchControl.setValue(this.engine.stretch);
      this.qControl.setValue(this.engine.Q);
      this.blackPointControl.setValue(this.engine.blackPoint * 10000);  // Convert to display scale
      this.blackRControl.setValue(this.engine.blackR * 10000);
      this.blackGControl.setValue(this.engine.blackG * 10000);
      this.blackBControl.setValue(this.engine.blackB * 10000);
      this.linkedCheckbox.checked = this.engine.linkedChannels;
      this.saturationControl.setValue(this.engine.saturation);
      this.clippingCombo.currentItem = this.engine.clippingMode;
      this.useActiveCheckbox.checked = this.engine.useActiveImage;
   };

   this.exportParameters = function()
   {
      Parameters.set("stretch", this.engine.stretch);
      Parameters.set("Q", this.engine.Q);
      Parameters.set("blackPoint", this.engine.blackPoint);
      Parameters.set("blackR", this.engine.blackR);
      Parameters.set("blackG", this.engine.blackG);
      Parameters.set("blackB", this.engine.blackB);
      Parameters.set("linkedChannels", this.engine.linkedChannels);
      Parameters.set("saturation", this.engine.saturation);
      Parameters.set("clippingMode", this.engine.clippingMode);
   };

   this.importParameters = function()
   {
      if (Parameters.has("stretch"))
         this.engine.stretch = Parameters.getReal("stretch");
      if (Parameters.has("Q"))
         this.engine.Q = Parameters.getReal("Q");
      if (Parameters.has("blackPoint"))
         this.engine.blackPoint = Parameters.getReal("blackPoint");
      if (Parameters.has("blackR"))
         this.engine.blackR = Parameters.getReal("blackR");
      if (Parameters.has("blackG"))
         this.engine.blackG = Parameters.getReal("blackG");
      if (Parameters.has("blackB"))
         this.engine.blackB = Parameters.getReal("blackB");
      if (Parameters.has("linkedChannels"))
         this.engine.linkedChannels = Parameters.getBoolean("linkedChannels");
      if (Parameters.has("saturation"))
         this.engine.saturation = Parameters.getReal("saturation");
      if (Parameters.has("clippingMode"))
         this.engine.clippingMode = Parameters.getInteger("clippingMode");

      this.updateControlsFromEngine();
   };

   this.apply = function()
   {
      if (!this.targetWindow)
      {
         (new MessageBox("No target image selected.", TITLE, StdIcon_Error, StdButton_Ok)).execute();
         return;
      }

      console.show();
      var result = this.engine.execute(this.targetWindow);
      if (result)
      {
         this.timeLabel.text = "Applied successfully";
      }
   };

   // Initialize
   this.populateImageLists();
   this.updateTargetWindow();
   this.updatePreviewModeButtons();

   // Import parameters if running from process icon
   if (Parameters.isViewTarget || Parameters.isGlobalTarget)
   {
      this.importParameters();
   }
}

LuptonDialog.prototype = new Dialog;

// ============================================================================
// Main Entry Point
// ============================================================================

function main()
{
   console.hide();

   // Check for an active image
   if (ImageWindow.activeWindow.isNull)
   {
      (new MessageBox(
         "Please open an RGB image before running this script.",
         TITLE,
         StdIcon_Error,
         StdButton_Ok
      )).execute();
      return;
   }

   var engine = new LuptonEngine();
   var dialog = new LuptonDialog(engine);

   dialog.execute();
}

main();
