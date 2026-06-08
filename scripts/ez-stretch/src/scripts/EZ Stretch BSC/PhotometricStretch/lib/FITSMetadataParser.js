/**
 * FITSMetadataParser.js
 *
 * Extract camera and filter information from FITS headers.
 * Handles various software conventions (NINA, SGP, APT, ASIAIR, etc.)
 */


/**
 * Primary FITS keywords to check first
 */
var PrimaryKeywords = {
   camera: ["INSTRUME", "CAMERA", "DETECTOR", "CCD_NAME", "CCD-NAME"],
   filter: ["FILTER", "FILTER1", "FILTER2", "FLTNAME", "FILTNAM"],
   gain: ["GAIN", "EGAIN", "GAINRAW", "CCD-GAIN"],
   offset: ["OFFSET", "BLKLEVEL", "PEDESSION"],
   exposure: ["EXPTIME", "EXPOSURE", "EXP_TIME"],
   temperature: ["CCD-TEMP", "CCDTEMP", "SET-TEMP", "TEMPERAT"],
   bayerPattern: ["BAYERPAT", "BAYER", "COLORTYP", "CFATYPE"],
   dateObs: ["DATE-OBS", "DATE_OBS", "DATEOBS"],
   binning: ["XBINNING", "BINNING"],
   xBinning: ["XBINNING", "XBIN"],
   yBinning: ["YBINNING", "YBIN"]
};

/**
 * Get a FITS keyword value from a view.
 *
 * @param {View} view - PixInsight View object
 * @param {string} name - Keyword name to find
 * @returns {string|null} Keyword value or null if not found
 */
function getFITSKeyword(view, name) {
   var keywords = view.window.keywords;
   for (var i = 0; i < keywords.length; i++) {
      if (keywords[i].name === name) {
         var value = keywords[i].value;
         // Strip quotes and trim whitespace
         if (typeof value === "string") {
            value = value.trim().replace(/^['"]|['"]$/g, "").trim();
         }
         return value;
      }
   }
   return null;
}

/**
 * Get a FITS keyword trying multiple alternative names.
 *
 * @param {View} view - PixInsight View object
 * @param {string[]} names - Array of keyword names to try
 * @returns {string|null} First found value or null
 */
function getFITSKeywordAlt(view, names) {
   for (var i = 0; i < names.length; i++) {
      var value = getFITSKeyword(view, names[i]);
      if (value !== null && value !== "") {
         return value;
      }
   }
   return null;
}

/**
 * Parse a numeric FITS value.
 *
 * @param {View} view - PixInsight View object
 * @param {string[]} names - Array of keyword names to try
 * @returns {number|null} Parsed number or null
 */
function getFITSNumber(view, names) {
   var value = getFITSKeywordAlt(view, names);
   if (value === null) return null;

   var num = parseFloat(value);
   return isNaN(num) ? null : num;
}

/**
 * Get all FITS keywords as a raw object.
 *
 * @param {View} view - PixInsight View object
 * @returns {Object} All keywords as name:value pairs
 */
function getAllFITSKeywords(view) {
   var result = {};
   var keywords = view.window.keywords;
   for (var i = 0; i < keywords.length; i++) {
      var kw = keywords[i];
      var value = kw.value;
      if (typeof value === "string") {
         value = value.trim().replace(/^['"]|['"]$/g, "").trim();
      }
      result[kw.name] = {
         value: value,
         comment: kw.comment || ""
      };
   }
   return result;
}

/**
 * Parse FITS metadata from a view.
 *
 * @param {View} view - PixInsight View object
 * @returns {Object} Parsed metadata object
 */
function parseFITSMetadata(view) {
   var metadata = {
      camera: null,
      filter: null,
      gain: null,
      offset: null,
      exposure: null,
      temperature: null,
      bayerPattern: null,
      dateObs: null,
      xBinning: null,
      yBinning: null,
      rawHeaders: {}
   };

   try {
      // Get all raw headers for debugging
      metadata.rawHeaders = getAllFITSKeywords(view);

      // Camera identification
      metadata.camera = getFITSKeywordAlt(view, PrimaryKeywords.camera);

      // Filter identification
      metadata.filter = getFITSKeywordAlt(view, PrimaryKeywords.filter);

      // Numeric values
      metadata.gain = getFITSNumber(view, PrimaryKeywords.gain);
      metadata.offset = getFITSNumber(view, PrimaryKeywords.offset);
      metadata.exposure = getFITSNumber(view, PrimaryKeywords.exposure);
      metadata.temperature = getFITSNumber(view, PrimaryKeywords.temperature);

      // Bayer pattern
      metadata.bayerPattern = getFITSKeywordAlt(view, PrimaryKeywords.bayerPattern);
      if (metadata.bayerPattern) {
         metadata.bayerPattern = metadata.bayerPattern.toUpperCase();
      }

      // Date/time
      metadata.dateObs = getFITSKeywordAlt(view, PrimaryKeywords.dateObs);

      // Binning
      metadata.xBinning = getFITSNumber(view, PrimaryKeywords.xBinning);
      metadata.yBinning = getFITSNumber(view, PrimaryKeywords.yBinning);

      // If only generic binning keyword exists
      if (metadata.xBinning === null) {
         var binning = getFITSNumber(view, PrimaryKeywords.binning);
         if (binning !== null) {
            metadata.xBinning = binning;
            metadata.yBinning = binning;
         }
      }

   } catch (e) {
      console.warningln("Error parsing FITS metadata: " + e.message);
   }

   return metadata;
}

/**
 * Check if camera string matches any of the given patterns.
 *
 * @param {string} cameraString - Camera name from FITS
 * @param {RegExp[]} patterns - Array of regex patterns to test
 * @returns {boolean} True if any pattern matches
 */
function matchesCameraPatterns(cameraString, patterns) {
   if (!cameraString) return false;
   var upper = cameraString.toUpperCase();
   for (var i = 0; i < patterns.length; i++) {
      if (patterns[i].test(upper)) return true;
   }
   return false;
}

/**
 * Check if a camera string indicates a mono camera.
 *
 * @param {string} cameraString - Camera name from FITS
 * @returns {boolean} True if likely mono camera
 */
function isMonoCamera(cameraString) {
   return matchesCameraPatterns(cameraString, [
      /MONO/,      // Contains "MONO"
      /\bMM\b/,    // Standalone "MM"
      /\b\d+M\b/,  // Model number + M (e.g., "2600M")
      /-M\s/,      // Hyphen-M-space
      /-M$/        // Ends with "-M"
   ]);
}

/**
 * Check if a camera string indicates an OSC (One Shot Color) camera.
 *
 * @param {string} cameraString - Camera name from FITS
 * @returns {boolean} True if likely OSC camera
 */
function isOSCCamera(cameraString) {
   return matchesCameraPatterns(cameraString, [
      /COLOR/,     // Contains "COLOR"
      /\bMC\b/,    // Standalone "MC"
      /\b\d+C\b/,  // Model number + C (e.g., "2600C")
      /-C\s/,      // Hyphen-C-space
      /-C$/,       // Ends with "-C"
      /OSC/        // Contains "OSC"
   ]);
}

/**
 * Valid Bayer pattern values
 */
var ValidBayerPatterns = ["RGGB", "BGGR", "GRBG", "GBRG"];

/**
 * Detect camera type from metadata.
 *
 * @param {Object} metadata - Parsed FITS metadata
 * @returns {string} "Mono", "OSC", or "Unknown"
 */
function detectCameraType(metadata) {
   // Check Bayer pattern first - must be exactly 4 chars and a valid pattern
   if (metadata.bayerPattern && metadata.bayerPattern.length === 4) {
      if (ValidBayerPatterns.indexOf(metadata.bayerPattern) !== -1) {
         return "OSC";
      }
      // Log warning for invalid patterns
      console.warningln("Invalid Bayer pattern '" + metadata.bayerPattern + "' ignored");
   }

   // Check camera name
   if (metadata.camera) {
      if (isMonoCamera(metadata.camera)) return "Mono";
      if (isOSCCamera(metadata.camera)) return "OSC";
   }

   return "Unknown";
}

/**
 * Format metadata as a readable string for display.
 *
 * @param {Object} metadata - Parsed FITS metadata
 * @returns {string} Formatted string
 */
function formatMetadata(metadata) {
   var lines = [];

   // Define fields to display: [key, label, suffix, formatter]
   var fields = [
      ["camera", "Camera", ""],
      ["filter", "Filter", ""],
      ["gain", "Gain", ""],
      ["offset", "Offset", ""],
      ["exposure", "Exposure", "s"],
      ["temperature", "Temperature", "\xB0C"],
      ["bayerPattern", "Bayer", ""],
      ["dateObs", "Date", ""]
   ];

   for (var i = 0; i < fields.length; i++) {
      var key = fields[i][0];
      var label = fields[i][1];
      var suffix = fields[i][2];
      var value = metadata[key];
      if (value !== null && value !== undefined) {
         lines.push(label + ": " + value + suffix);
      }
   }

   // Special handling for binning
   if (metadata.xBinning !== null) {
      lines.push("Binning: " + metadata.xBinning + "x" + (metadata.yBinning || metadata.xBinning));
   }

   return lines.join("\n");
}

/**
 * Log metadata to console.
 *
 * @param {Object} metadata - Parsed FITS metadata
 */
function logMetadata(metadata) {
   console.writeln("=== FITS Metadata ===");
   console.writeln(formatMetadata(metadata));
   console.writeln("Camera type: " + detectCameraType(metadata));
   console.writeln("====================");
}
