/**
 * LuptonStretch.js
 *
 * Core Lupton RGB asinh stretch implementation for PixInsight.
 * Based on Lupton et al. (2004) "Preparing Red-Green-Blue Images from CCD Data"
 *
 * The key insight: stretch the weighted luminance, then scale all RGB channels
 * by the same factor to preserve color ratios.
 */


/**
 * Default stretch parameters
 */
var LuptonDefaults = {
   softening: 25.0,       // Q parameter - controls linear/log transition (0 to 100)
   Q: 100.0,              // Stretch α - linear amplification (0.1 to 1000, log scale)
   blackPoint: 0.0,       // Background level to subtract (0 to 0.01)
   targetBackground: 0.15, // Desired output background level
   saturationBoost: 1.0   // Post-stretch saturation multiplier (0.5 to 2.0)
};

/**
 * Default RGB weights (Rec.709 luminance)
 */
var DefaultWeights = {
   R: 0.2126,
   G: 0.7152,
   B: 0.0722
};

/**
 * Extract and merge parameters with defaults.
 *
 * @param {Object} params - User-provided parameters
 * @returns {Object} Merged parameters with all fields populated
 */
function mergeParams(params) {
   return {
      weights: params.weights || DefaultWeights,
      softening: (params.softening !== undefined) ? params.softening : LuptonDefaults.softening,
      Q: (params.Q !== undefined) ? params.Q : LuptonDefaults.Q,
      blackPoint: (params.blackPoint !== undefined) ? params.blackPoint : LuptonDefaults.blackPoint,
      targetBackground: (params.targetBackground !== undefined) ? params.targetBackground : LuptonDefaults.targetBackground,
      saturationBoost: (params.saturationBoost !== undefined) ? params.saturationBoost : LuptonDefaults.saturationBoost
   };
}

/**
 * Apply Lupton asinh stretch to an image.
 *
 * @param {View} view - PixInsight View object
 * @param {Object} params - Stretch parameters
 * @param {Object} params.weights - RGB weights {R, G, B} summing to 1.0
 * @param {number} params.softening - 'a' parameter (default 0.1)
 * @param {number} params.Q - Stretch intensity (default 1.0)
 * @param {number} params.blackPoint - Background to subtract (default 0.0)
 * @param {number} params.targetBackground - Target background level (default 0.15)
 * @param {number} params.saturationBoost - Saturation multiplier (default 1.0)
 */
function applyLuptonStretch(view, params) {
   var p = mergeParams(params);
   var image = view.image;

   // Validate RGB image
   if (image.numberOfChannels < 3) {
      console.criticalln("LuptonStretch requires an RGB image (3 channels)");
      return false;
   }

   // Validate parameters to prevent division by zero
   if (p.softening <= 0 || p.Q <= 0) {
      console.criticalln("LuptonStretch: softening and Q must be > 0");
      return false;
   }

   var width = image.width;
   var height = image.height;
   var numPixels = width * height;
   var rect = new Rect(0, 0, width, height);

   // Allocate sample arrays
   var R = new Float64Array(numPixels);
   var G = new Float64Array(numPixels);
   var B = new Float64Array(numPixels);

   // Read pixel data
   image.getSamples(R, rect, 0);
   image.getSamples(G, rect, 1);
   image.getSamples(B, rect, 2);

   // Pre-calculate normalization factor
   var normFactor = 1.0 / Math.asinh(p.Q / p.softening);

   // Process all pixels
   view.beginProcess();
   try {
      for (var i = 0; i < numPixels; i++) {
         // Subtract black point
         var r = Math.max(0, R[i] - p.blackPoint);
         var g = Math.max(0, G[i] - p.blackPoint);
         var b = Math.max(0, B[i] - p.blackPoint);

         // Compute weighted intensity
         var I = p.weights.R * r + p.weights.G * g + p.weights.B * b;

         // Handle near-zero intensity to avoid division issues
         if (I < 1e-10) {
            R[i] = 0;
            G[i] = 0;
            B[i] = 0;
            continue;
         }

         // Apply asinh stretch to intensity
         var stretchedI = Math.asinh(p.Q * I / p.softening) * normFactor;
         var scale = stretchedI / I;

         // Apply scale to all channels
         var rOut = r * scale;
         var gOut = g * scale;
         var bOut = b * scale;

         // Apply saturation boost if needed
         if (p.saturationBoost !== 1.0) {
            var lum = p.weights.R * rOut + p.weights.G * gOut + p.weights.B * bOut;
            rOut = lum + p.saturationBoost * (rOut - lum);
            gOut = lum + p.saturationBoost * (gOut - lum);
            bOut = lum + p.saturationBoost * (bOut - lum);
         }

         // Shift to target background (apply to dark pixels proportionally)
         if (p.targetBackground > 0) {
            var bgShift = p.targetBackground * (1.0 - stretchedI);
            rOut += bgShift;
            gOut += bgShift;
            bOut += bgShift;
         }

         // Clip to valid range
         R[i] = Math.max(0, Math.min(1, rOut));
         G[i] = Math.max(0, Math.min(1, gOut));
         B[i] = Math.max(0, Math.min(1, bOut));
      }

      // Write back modified samples
      image.setSamples(R, rect, 0);
      image.setSamples(G, rect, 1);
      image.setSamples(B, rect, 2);

   } finally {
      view.endProcess();
   }

   console.writeln("LuptonStretch applied successfully");
   console.writeln("  Weights: R=" + p.weights.R.toFixed(3) + " G=" + p.weights.G.toFixed(3) + " B=" + p.weights.B.toFixed(3));
   console.writeln("  Q=" + p.Q.toFixed(2) + " softening=" + p.softening.toFixed(3) + " blackPoint=" + p.blackPoint.toFixed(4));

   return true;
}

/**
 * Apply stretch as Screen Transfer Function (STF) preview only.
 * Non-destructive - doesn't modify the image.
 *
 * @param {View} view - PixInsight View object
 * @param {Object} params - Stretch parameters (same as applyLuptonStretch)
 */
function previewStretch(view, params) {
   var p = mergeParams(params);

   // Calculate STF parameters that approximate the Lupton stretch
   // The midtones balance (m) controls where middle gray falls
   var m = 0.5 * (1.0 - Math.asinh(p.Q * 0.18 / p.softening) / Math.asinh(p.Q / p.softening));
   m = Math.max(0.0001, Math.min(0.9999, m));

   // Shadow clipping approximates black point
   var c = p.blackPoint;

   // Highlights - leave at 1.0 for no clipping
   var h = 1.0;

   // Create STF instance
   var stf = new ScreenTransferFunction;

   // Set STF for each channel (using same curve for all to preserve color)
   stf.STF = [
      [c, m, h, 0, 1],  // R
      [c, m, h, 0, 1],  // G
      [c, m, h, 0, 1],  // B
      [c, m, h, 0, 1]   // L (combined)
   ];

   // Apply STF to the view
   try {
      stf.executeOn(view);
   } catch (e) {
      console.criticalln("Failed to apply STF preview: " + e.message);
      console.criticalln("The image may have been modified or closed.");
      return false;
   }

   console.writeln("STF preview applied (m=" + m.toFixed(4) + ", c=" + c.toFixed(4) + ")");

   return true;
}

/**
 * Auto-calculate optimal stretch parameters from image statistics.
 * Uses QE-weighted intensity for accurate luminance calculation.
 *
 * Parameter ranges:
 *   - Stretch (α/Q): 0.1 to 1000 - linear amplification factor (logarithmic slider)
 *   - Q (softening): 0 to 100 - controls linear/log transition
 *   - Black Point: 0 to 0.01 - background subtraction
 *
 * @param {View} view - PixInsight View object
 * @param {Object} weights - RGB weights for intensity calculation (from QE/filter data)
 * @returns {Object} Calculated parameters {softening, Q, blackPoint}
 */
function autoCalculateParams(view, weights) {
   weights = weights || DefaultWeights;

   var image = view.image;
   var width = image.width;
   var height = image.height;
   var numPixels = width * height;

   // Handle empty image
   if (numPixels === 0) {
      console.warningln("autoCalculateParams: Empty image, using defaults");
      return {
         softening: LuptonDefaults.softening,
         Q: LuptonDefaults.Q,
         blackPoint: 0,
         stats: {}
      };
   }

   var rect = new Rect(0, 0, width, height);

   // Read pixel data
   var R = new Float64Array(numPixels);
   var G = new Float64Array(numPixels);
   var B = new Float64Array(numPixels);

   image.getSamples(R, rect, 0);
   image.getSamples(G, rect, 1);
   image.getSamples(B, rect, 2);

   // Calculate QE-weighted intensity for all pixels
   // This uses the sensor/filter-specific weights for accurate luminance
   var intensities = new Float64Array(numPixels);
   for (var i = 0; i < numPixels; i++) {
      intensities[i] = weights.R * R[i] + weights.G * G[i] + weights.B * B[i];
   }

   // Sort intensities to get statistics
   var sorted = [];
   for (var j = 0; j < numPixels; j++) {
      sorted[j] = intensities[j];
   }
   sorted.sort(function(a, b) { return a - b; });

   // Calculate percentiles
   var p01 = sorted[Math.floor(numPixels * 0.01)];   // 1st percentile (shadows)
   var p10 = sorted[Math.floor(numPixels * 0.10)];   // 10th percentile
   var p50 = sorted[Math.floor(numPixels * 0.50)];   // Median
   var p90 = sorted[Math.floor(numPixels * 0.90)];   // 90th percentile
   var p99 = sorted[Math.floor(numPixels * 0.99)];   // 99th percentile (highlights)

   // Calculate MAD (Median Absolute Deviation) for robust noise estimate
   var mad = 0;
   for (var i = 0; i < numPixels; i++) {
      mad += Math.abs(intensities[i] - p50);
   }
   mad /= numPixels;

   // Black point: conservative estimate to avoid clipping signal
   // Clamp to 0-0.01 range
   var bottomMedian = sorted[Math.floor(numPixels * 0.05)];
   var robustEstimate = Math.max(0, p50 - 2.8 * mad);
   var blackPoint = Math.max(robustEstimate, bottomMedian * 0.8);
   blackPoint = Math.max(0, Math.min(0.01, blackPoint));  // Clamp to 0-0.01

   // Dynamic range after black point subtraction
   var dynamicRange = p99 - blackPoint;

   // Q (Softening): controls linear/log transition
   // Range: 0 to 100
   // Higher values = gentler stretch (more linear), lower = more aggressive (more log)
   // Base on median position in dynamic range
   var medianRelative = (p50 - blackPoint) / Math.max(dynamicRange, 1e-10);
   // Map to Q range: very dark images get lower Q (more aggressive), bright get higher
   // Linear images (already stretched) get higher Q
   var softening = 10 + 40 * Math.pow(medianRelative, 0.5);  // Range ~10-50
   if (p50 > 0.3) {
      // Already stretched - use gentler settings
      softening = 40 + 40 * medianRelative;  // Range ~40-80
   }
   softening = Math.max(0, Math.min(100, softening));

   // Stretch (α/Q): linear amplification factor
   // Range: 0.1 to 1000 (logarithmic slider)
   // Calculate based on how much amplification is needed
   // For linear astronomical data, typically need 100-500
   var stretchAlpha;
   if (dynamicRange > 0 && dynamicRange < 1) {
      // Estimate stretch needed to bring median to ~0.2
      var targetMedian = 0.2;
      var currentMedian = Math.max(p50 - blackPoint, 1e-10);
      stretchAlpha = targetMedian / currentMedian;
      // Apply asinh correction factor (use softening scaled to algorithm's internal range)
      var internalSoftening = softening / 10;  // Scale 0-100 to ~0-10 for algorithm
      stretchAlpha = stretchAlpha * Math.asinh(internalSoftening) / Math.max(internalSoftening, 0.1);
   } else {
      stretchAlpha = 100;  // Default
   }
   // Clamp to valid range
   stretchAlpha = Math.max(0.1, Math.min(1000, stretchAlpha));

   // If image is already stretched (high median), reduce stretch
   if (p50 > 0.3) {
      stretchAlpha = Math.min(stretchAlpha, 50);
   }

   var result = {
      softening: softening,
      Q: stretchAlpha,
      blackPoint: blackPoint,
      // Also return statistics for UI display
      stats: {
         p01: p01,
         p10: p10,
         p50: p50,
         p90: p90,
         p99: p99,
         mad: mad,
         dynamicRange: dynamicRange
      }
   };

   console.writeln("Auto-calculated parameters:");
   console.writeln("  Black Point: " + blackPoint.toFixed(6) + " (range 0-0.01)");
   console.writeln("  Q (Softening): " + softening.toFixed(1) + " (range 0-100)");
   console.writeln("  Stretch (α): " + stretchAlpha.toFixed(1) + " (range 0.1-1000, log scale)");
   console.writeln("  Image stats: median=" + p50.toFixed(6) + ", MAD=" + mad.toFixed(6) + ", DR=" + dynamicRange.toFixed(4));
   console.writeln("  Using QE weights: R=" + weights.R.toFixed(3) + " G=" + weights.G.toFixed(3) + " B=" + weights.B.toFixed(3));

   return result;
}

/**
 * Apply stretch with tiled processing for large images.
 * Reduces memory usage by processing in chunks.
 *
 * @param {View} view - PixInsight View object
 * @param {Object} params - Stretch parameters
 * @param {number} tileSize - Size of processing tiles (default 1024)
 */
function applyLuptonStretchTiled(view, params, tileSize) {
   tileSize = tileSize || 1024;
   var p = mergeParams(params);
   var image = view.image;

   if (image.numberOfChannels < 3) {
      console.criticalln("LuptonStretch requires an RGB image (3 channels)");
      return false;
   }

   if (p.softening <= 0 || p.Q <= 0) {
      console.criticalln("LuptonStretch: softening and Q must be > 0");
      return false;
   }

   var width = image.width;
   var height = image.height;
   var normFactor = 1.0 / Math.asinh(p.Q / p.softening);

   view.beginProcess();
   try {
      for (var y = 0; y < height; y += tileSize) {
         var tileHeight = Math.min(tileSize, height - y);

         for (var x = 0; x < width; x += tileSize) {
            var tileWidth = Math.min(tileSize, width - x);
            var rect = new Rect(x, y, x + tileWidth, y + tileHeight);
            var numPixels = tileWidth * tileHeight;

            var R = new Float64Array(numPixels);
            var G = new Float64Array(numPixels);
            var B = new Float64Array(numPixels);

            image.getSamples(R, rect, 0);
            image.getSamples(G, rect, 1);
            image.getSamples(B, rect, 2);

            for (var i = 0; i < numPixels; i++) {
               var r = Math.max(0, R[i] - p.blackPoint);
               var g = Math.max(0, G[i] - p.blackPoint);
               var b = Math.max(0, B[i] - p.blackPoint);

               var I = p.weights.R * r + p.weights.G * g + p.weights.B * b;

               if (I < 1e-10) {
                  R[i] = 0;
                  G[i] = 0;
                  B[i] = 0;
                  continue;
               }

               var stretchedI = Math.asinh(p.Q * I / p.softening) * normFactor;
               var scale = stretchedI / I;

               var rOut = r * scale;
               var gOut = g * scale;
               var bOut = b * scale;

               if (p.saturationBoost !== 1.0) {
                  var lum = p.weights.R * rOut + p.weights.G * gOut + p.weights.B * bOut;
                  rOut = lum + p.saturationBoost * (rOut - lum);
                  gOut = lum + p.saturationBoost * (gOut - lum);
                  bOut = lum + p.saturationBoost * (bOut - lum);
               }

               if (p.targetBackground > 0) {
                  var bgShift = p.targetBackground * (1.0 - stretchedI);
                  rOut += bgShift;
                  gOut += bgShift;
                  bOut += bgShift;
               }

               R[i] = Math.max(0, Math.min(1, rOut));
               G[i] = Math.max(0, Math.min(1, gOut));
               B[i] = Math.max(0, Math.min(1, bOut));
            }

            // Write tile back
            image.setSamples(R, rect, 0);
            image.setSamples(G, rect, 1);
            image.setSamples(B, rect, 2);
         }

         // Progress indication
         console.write("<end>\rProcessing: " + Math.round(100 * (y + tileHeight) / height) + "%");
      }
      console.writeln("");

   } finally {
      view.endProcess();
   }

   console.writeln("LuptonStretch (tiled) applied successfully");
   return true;
}

/**
 * Calculate stretch curve for a given intensity value.
 * Useful for plotting/visualization.
 *
 * @param {number} I - Input intensity (0-1)
 * @param {number} Q - Stretch intensity
 * @param {number} softening - Softening parameter
 * @returns {number} Stretched intensity
 */
function stretchCurve(I, Q, softening) {
   if (I < 1e-10) return 0;
   var normFactor = 1.0 / Math.asinh(Q / softening);
   return Math.asinh(Q * I / softening) * normFactor;
}

/**
 * Generate stretch curve lookup table for fast processing.
 *
 * @param {number} Q - Stretch intensity
 * @param {number} softening - Softening parameter
 * @param {number} resolution - LUT resolution (default 65536)
 * @returns {Float64Array} Lookup table
 */
function generateStretchLUT(Q, softening, resolution) {
   resolution = resolution || 65536;
   var lut = new Float64Array(resolution);
   var normFactor = 1.0 / Math.asinh(Q / softening);

   for (var i = 0; i < resolution; i++) {
      var I = i / (resolution - 1);
      if (I < 1e-10) {
         lut[i] = 0;
      } else {
         lut[i] = Math.asinh(Q * I / softening) * normFactor;
      }
   }

   return lut;
}
