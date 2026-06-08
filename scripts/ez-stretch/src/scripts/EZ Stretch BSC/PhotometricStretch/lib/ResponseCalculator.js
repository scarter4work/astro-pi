/**
 * ResponseCalculator.js
 *
 * Compute RGB weights from sensor QE and filter transmission curves.
 * Handles OSC cameras with Bayer CFA, mono cameras, and various filter types.
 */


/**
 * Approximate Bayer CFA micro-filter spectral responses.
 * These represent typical responses; actual values vary by sensor.
 */
var BayerCFA = {
   RED: {
      wavelength_nm: [400, 450, 500, 550, 580, 600, 620, 650, 700, 750, 800],
      response:      [0.02, 0.03, 0.05, 0.08, 0.15, 0.35, 0.65, 0.90, 0.95, 0.85, 0.70]
   },
   GREEN: {
      wavelength_nm: [400, 450, 480, 500, 520, 540, 560, 580, 600, 650, 700, 750],
      response:      [0.05, 0.12, 0.30, 0.55, 0.80, 0.95, 0.92, 0.75, 0.45, 0.15, 0.05, 0.02]
   },
   BLUE: {
      wavelength_nm: [400, 420, 450, 470, 490, 510, 530, 550, 580, 600, 650],
      response:      [0.60, 0.80, 0.95, 0.90, 0.75, 0.50, 0.25, 0.12, 0.05, 0.02, 0.01]
   }
};

/**
 * Linear interpolation helper.
 *
 * @param {number} x - Value to interpolate at
 * @param {number} x0 - Lower x bound
 * @param {number} x1 - Upper x bound
 * @param {number} y0 - Lower y value
 * @param {number} y1 - Upper y value
 * @returns {number} Interpolated y value
 */
function lerp(x, x0, x1, y0, y1) {
   if (x1 === x0) return y0;
   return y0 + (y1 - y0) * (x - x0) / (x1 - x0);
}

/**
 * Interpolate a spectral curve at a specific wavelength.
 *
 * @param {Object} curve - Curve with wavelength_nm and response/qe_percent arrays
 * @param {number} wavelength - Wavelength in nm
 * @param {string} valueKey - Key for values ("response", "qe_percent", "transmission_percent")
 * @returns {number} Interpolated value (0-1 scale)
 */
function interpolateCurve(curve, wavelength, valueKey) {
   var wavelengths = curve.wavelength_nm;
   var values = curve[valueKey];

   if (!wavelengths || !values || wavelengths.length === 0) {
      return 0;
   }

   // Scale factor for percent to decimal
   var scale = valueKey.indexOf("percent") !== -1 ? 0.01 : 1.0;

   // Below range
   if (wavelength <= wavelengths[0]) {
      return values[0] * scale;
   }

   // Above range
   if (wavelength >= wavelengths[wavelengths.length - 1]) {
      return values[values.length - 1] * scale;
   }

   // Find bracketing indices and interpolate
   for (var i = 0; i < wavelengths.length - 1; i++) {
      if (wavelength >= wavelengths[i] && wavelength <= wavelengths[i + 1]) {
         return lerp(wavelength, wavelengths[i], wavelengths[i + 1],
                     values[i], values[i + 1]) * scale;
      }
   }

   return 0;
}

/**
 * Interpolate sensor QE at a specific wavelength.
 *
 * @param {Object} sensor - Sensor object with qe_curve
 * @param {number} wavelength - Wavelength in nm
 * @returns {number} QE as decimal (0-1)
 */
function interpolateQE(sensor, wavelength) {
   if (!sensor || !sensor.qe_curve) {
      console.warningln("interpolateQE: No sensor QE data, using 50% default");
      return 0.5;
   }
   return interpolateCurve(sensor.qe_curve, wavelength, "qe_percent");
}

/**
 * Calculate Gaussian response at a wavelength.
 *
 * @param {number} wavelength - Wavelength in nm
 * @param {number} center - Center wavelength
 * @param {number} fwhm - Full width at half maximum
 * @param {number} peak - Peak transmission value
 * @returns {number} Transmission value
 */
function gaussianResponse(wavelength, center, fwhm, peak) {
   var sigma = fwhm / 2.355;  // FWHM to sigma conversion
   var exponent = -0.5 * Math.pow((wavelength - center) / sigma, 2);
   return peak * Math.exp(exponent);
}

/**
 * Get filter transmission at a specific wavelength.
 *
 * @param {Object} filter - Filter object with transmission_curve
 * @param {number} wavelength - Wavelength in nm
 * @returns {number} Transmission as decimal (0-1)
 */
function getFilterTransmission(filter, wavelength) {
   if (!filter || !filter.transmission_curve) return 1.0;

   var curve = filter.transmission_curve;

   // Gaussian and bandpass use same calculation
   if (curve.type === "gaussian" || curve.type === "bandpass") {
      var defaultPeak = curve.type === "gaussian" ? 0.9 : 0.95;
      return gaussianResponse(wavelength, curve.center_nm, curve.fwhm_nm,
                              curve.peak_transmission || defaultPeak);
   }

   // Flat transmission in range
   if (curve.type === "flat") {
      var range = curve.wavelength_range_nm || [400, 700];
      if (wavelength >= range[0] && wavelength <= range[1]) {
         return (curve.transmission_percent || 95) * 0.01;
      }
      return 0;
   }

   // Sampled curve (multi-bandpass, notch-filter, or default)
   if (curve.wavelength_nm && curve.transmission_percent) {
      return interpolateCurve(curve, wavelength, "transmission_percent");
   }

   return 1.0;
}

/**
 * Get Bayer CFA response for a channel at a wavelength.
 *
 * @param {string} channel - "R", "G", or "B"
 * @param {number} wavelength - Wavelength in nm
 * @returns {number} Response (0-1)
 */
function getCFAResponse(channel, wavelength) {
   var cfa;
   switch (channel.toUpperCase()) {
      case "R": cfa = BayerCFA.RED; break;
      case "G": cfa = BayerCFA.GREEN; break;
      case "B": cfa = BayerCFA.BLUE; break;
      default: return 0;
   }
   return interpolateCurve(cfa, wavelength, "response");
}

/**
 * Integrate combined response for one channel.
 *
 * @param {Object} sensor - Sensor object
 * @param {Object} filter - Filter object (or null)
 * @param {string} channel - "R", "G", or "B"
 * @param {number} startWl - Start wavelength (default 380)
 * @param {number} endWl - End wavelength (default 750)
 * @param {number} step - Integration step (default 1)
 * @returns {number} Integrated response
 */
function integrateChannelResponse(sensor, filter, channel, startWl, endWl, step) {
   startWl = startWl || 380;
   endWl = endWl || 750;
   step = step || 1;

   var integral = 0;

   for (var wl = startWl; wl <= endWl; wl += step) {
      var qe = interpolateQE(sensor, wl);
      var filterT = getFilterTransmission(filter, wl);
      var cfaR = getCFAResponse(channel, wl);

      integral += qe * filterT * cfaR * step;
   }

   return integral;
}

/**
 * Calculate RGB weights for an OSC camera with Bayer CFA.
 *
 * @param {Object} sensor - Sensor object with QE curve
 * @param {Object} filter - Filter object (or null for no filter)
 * @returns {Object} Weights {R, G, B} normalized to sum to 1.0
 */
function calculateOSCWeights(sensor, filter) {
   var weightR = integrateChannelResponse(sensor, filter, "R");
   var weightG = integrateChannelResponse(sensor, filter, "G");
   var weightB = integrateChannelResponse(sensor, filter, "B");

   // Normalize
   var total = weightR + weightG + weightB;
   if (isNaN(total) || total < 1e-10) {
      // Fallback to equal weights if integration failed
      console.warningln("calculateOSCWeights: Integration failed (total=" + total + "), using equal weights");
      return { R: 0.333, G: 0.334, B: 0.333 };
   }

   return {
      R: weightR / total,
      G: weightG / total,
      B: weightB / total
   };
}

/**
 * Calculate RGB weights for a mono camera.
 * For mono cameras, all channels have equal weight since there's no Bayer CFA.
 *
 * @param {Object} sensor - Sensor object (not used, but kept for API consistency)
 * @param {Object} filter - Filter object (not used for mono)
 * @returns {Object} Equal weights {R: 0.333, G: 0.334, B: 0.333}
 */
function calculateMonoWeights(sensor, filter) {
   // Mono cameras don't have RGB channels in the same sense
   // Return equal weights for consistency with RGB processing
   return {
      R: 0.333,
      G: 0.334,
      B: 0.333
   };
}

/**
 * Calculate RGB weights based on filter's channel mapping.
 * Used for narrowband filters with defined RGB channel assignments.
 *
 * @param {Object} filter - Filter object with rgb_channel_mapping
 * @returns {Object|null} Weights based on channel mapping or null
 */
function calculateNarrowbandWeights(filter) {
   if (!filter || !filter.rgb_channel_mapping) return null;

   var mapping = filter.rgb_channel_mapping;
   var weights = { R: 0, G: 0, B: 0 };

   // Count emissions per channel
   var rCount = mapping.R ? mapping.R.length : 0;
   var gCount = mapping.G ? mapping.G.length : 0;
   var bCount = mapping.B ? mapping.B.length : 0;
   var total = rCount + gCount + bCount;

   if (total === 0) return null;

   // Weight by number of emission lines captured
   weights.R = rCount / total;
   weights.G = gCount / total;
   weights.B = bCount / total;

   return weights;
}

/**
 * Calculate RGB weights from sensor and filter data.
 * Main entry point for weight calculation.
 *
 * @param {Object} sensor - Sensor object from database
 * @param {Object} filter - Filter object from database (or null)
 * @param {boolean} isMono - True if mono camera
 * @returns {Object} Weights {R, G, B} normalized to sum to 1.0
 */
function calculateRGBWeights(sensor, filter, isMono) {
   // Mono camera: use equal weights
   if (isMono) {
      return calculateMonoWeights(sensor, filter);
   }

   // Check for pre-calculated weights in sensor
   if (sensor && sensor.qe_rgb_weights && !filter) {
      var preCalc = sensor.qe_rgb_weights;
      // Ensure normalization (guard against zero/NaN total)
      var total = preCalc.R + preCalc.G + preCalc.B;
      if (isNaN(total) || total < 1e-10) {
         console.warningln("Pre-calculated weights invalid (total=" + total + "), using equal weights");
         return { R: 0.333, G: 0.334, B: 0.333 };
      }
      return {
         R: preCalc.R / total,
         G: preCalc.G / total,
         B: preCalc.B / total
      };
   }

   // For narrowband filters with channel mapping, use that
   if (filter && filter.rgb_channel_mapping) {
      var nbWeights = calculateNarrowbandWeights(filter);
      if (nbWeights) {
         console.writeln("Using narrowband channel mapping for weights");
         return nbWeights;
      }
   }

   // Full integration for OSC with QE and filter data
   if (sensor && sensor.qe_curve) {
      return calculateOSCWeights(sensor, filter);
   }

   // Fallback to default Rec.709 weights
   console.writeln("Using default Rec.709 weights (no sensor data)");
   return {
      R: 0.2126,
      G: 0.7152,
      B: 0.0722
   };
}

/**
 * Format weights for display.
 *
 * @param {Object} weights - Weights {R, G, B}
 * @returns {string} Formatted string
 */
function formatWeights(weights) {
   return "R: " + weights.R.toFixed(3) +
          "  G: " + weights.G.toFixed(3) +
          "  B: " + weights.B.toFixed(3);
}

/**
 * Generate spectral response data for visualization.
 *
 * @param {Object} sensor - Sensor object
 * @param {Object} filter - Filter object
 * @param {number} startWl - Start wavelength
 * @param {number} endWl - End wavelength
 * @param {number} step - Step size
 * @returns {Object} Response curves for plotting
 */
function generateResponseCurves(sensor, filter, startWl, endWl, step) {
   startWl = startWl || 380;
   endWl = endWl || 750;
   step = step || 5;

   var wavelengths = [];
   var qe = [];
   var filterT = [];
   var combinedR = [];
   var combinedG = [];
   var combinedB = [];

   for (var wl = startWl; wl <= endWl; wl += step) {
      wavelengths.push(wl);

      var qeVal = interpolateQE(sensor, wl);
      var filterVal = getFilterTransmission(filter, wl);

      qe.push(qeVal);
      filterT.push(filterVal);

      combinedR.push(qeVal * filterVal * getCFAResponse("R", wl));
      combinedG.push(qeVal * filterVal * getCFAResponse("G", wl));
      combinedB.push(qeVal * filterVal * getCFAResponse("B", wl));
   }

   return {
      wavelengths: wavelengths,
      qe: qe,
      filter: filterT,
      combinedR: combinedR,
      combinedG: combinedG,
      combinedB: combinedB
   };
}
