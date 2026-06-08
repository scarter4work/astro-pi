/**
 * HardwareMatcher.js
 *
 * Match FITS metadata strings to database entries using multiple strategies:
 * exact match, alias match, glob pattern match, and fuzzy matching.
 */


/**
 * Normalize a string for fuzzy comparison.
 * Removes spaces, hyphens, underscores, and common suffixes.
 *
 * @param {string} str - Input string
 * @returns {string} Normalized string
 */
function normalizeString(str) {
   if (!str) return "";

   var normalized = str.toLowerCase();

   // Remove common separators
   normalized = normalized.replace(/[\s\-_\.]/g, "");

   // Remove common suffixes that don't affect identification
   var suffixes = ["pro", "cooled", "color", "mono", "cool", "uncooled"];
   for (var i = 0; i < suffixes.length; i++) {
      var suffix = suffixes[i];
      if (normalized.endsWith(suffix)) {
         normalized = normalized.slice(0, -suffix.length);
      }
   }

   return normalized;
}

/**
 * Calculate Levenshtein distance between two strings.
 *
 * @param {string} a - First string
 * @param {string} b - Second string
 * @returns {number} Edit distance
 */
function levenshteinDistance(a, b) {
   if (a.length === 0) return b.length;
   if (b.length === 0) return a.length;

   var matrix = [];

   // Initialize matrix
   for (var i = 0; i <= b.length; i++) {
      matrix[i] = [i];
   }
   for (var j = 0; j <= a.length; j++) {
      matrix[0][j] = j;
   }

   // Fill matrix
   for (var i = 1; i <= b.length; i++) {
      for (var j = 1; j <= a.length; j++) {
         if (b.charAt(i - 1) === a.charAt(j - 1)) {
            matrix[i][j] = matrix[i - 1][j - 1];
         } else {
            matrix[i][j] = Math.min(
               matrix[i - 1][j - 1] + 1,  // substitution
               matrix[i][j - 1] + 1,      // insertion
               matrix[i - 1][j] + 1       // deletion
            );
         }
      }
   }

   return matrix[b.length][a.length];
}

/**
 * Calculate normalized similarity score (0-1).
 *
 * @param {string} a - First string
 * @param {string} b - Second string
 * @returns {number} Similarity score (1 = identical)
 */
function stringSimilarity(a, b) {
   var normA = normalizeString(a);
   var normB = normalizeString(b);

   if (normA === normB) return 1.0;
   if (normA.length === 0 || normB.length === 0) return 0.0;

   var distance = levenshteinDistance(normA, normB);
   var maxLen = Math.max(normA.length, normB.length);

   return 1 - (distance / maxLen);
}

/**
 * Check if a string matches a glob pattern.
 * Supports * as wildcard for any characters.
 *
 * @param {string} str - String to test
 * @param {string} pattern - Glob pattern (e.g., "*ASI2600*")
 * @returns {boolean} True if matches
 */
function globMatch(str, pattern) {
   if (!str || !pattern) return false;

   // Convert glob to regex
   var regexStr = pattern
      .replace(/[.+^${}()|[\]\\]/g, "\\$&")  // Escape regex special chars
      .replace(/\*/g, ".*")                   // * becomes .*
      .replace(/\?/g, ".");                   // ? becomes .

   var regex = new RegExp("^" + regexStr + "$", "i");
   return regex.test(str);
}

/**
 * Extract model numbers from a string for comparison.
 *
 * @param {string} str - Input string
 * @returns {string[]} Array of model numbers found
 */
function extractModelNumbers(str) {
   if (!str) return [];
   var matches = str.match(/\d{3,4}/g);
   return matches || [];
}

/**
 * Check if two strings share common model numbers.
 *
 * @param {string} a - First string
 * @param {string} b - Second string
 * @returns {boolean} True if they share a model number
 */
function hasCommonModelNumber(a, b) {
   var numsA = extractModelNumbers(a);
   var numsB = extractModelNumbers(b);

   for (var i = 0; i < numsA.length; i++) {
      for (var j = 0; j < numsB.length; j++) {
         if (numsA[i] === numsB[j]) {
            return true;
         }
      }
   }
   return false;
}

/**
 * Calculate match confidence for a query against an entry.
 * Centralizes matching strategy logic for both cameras and filters.
 *
 * @param {string} query - String to match (from FITS header)
 * @param {string} entryName - Canonical name in database
 * @param {Object} entry - Database entry with aliases/fits_patterns
 * @param {boolean} useModelNumbers - Whether to use model number matching
 * @returns {number} Confidence score 0-1
 */
function calculateMatchConfidence(query, entryName, entry, useModelNumbers) {
   // Strategy 1: Exact match
   if (query === entryName) return 1.0;

   // Strategy 2: Case-insensitive exact match
   if (query.toLowerCase() === entryName.toLowerCase()) return 0.98;

   // Strategy 3: Alias match
   if (entry.aliases) {
      for (var i = 0; i < entry.aliases.length; i++) {
         var alias = entry.aliases[i].toLowerCase();
         if (query.toLowerCase() === alias) return 0.95;
         // Partial alias match (for filters)
         if (query.toLowerCase().indexOf(alias) !== -1) return 0.85;
      }
   }

   // Strategy 4: FITS pattern match
   if (entry.fits_patterns) {
      for (var i = 0; i < entry.fits_patterns.length; i++) {
         if (globMatch(query, entry.fits_patterns[i])) return 0.85;
      }
   }

   // Strategy 5: Model number match + fuzzy (cameras only)
   if (useModelNumbers && hasCommonModelNumber(query, entryName)) {
      var similarity = stringSimilarity(query, entryName);
      return 0.5 + (similarity * 0.3);
   }

   // Strategy 6: Pure fuzzy match
   var similarity = stringSimilarity(query, entryName);
   var threshold = useModelNumbers ? 0.7 : 0.6;
   var multiplier = useModelNumbers ? 0.6 : 0.7;
   if (similarity > threshold) {
      return similarity * multiplier;
   }

   return 0;
}

/**
 * Match a camera string against the camera database.
 *
 * @param {string} cameraString - Camera name from FITS header
 * @param {Object} cameraDatabase - Camera database object
 * @param {Object} sensorDatabase - Sensor database object
 * @returns {Object|null} Match result {camera, sensor, confidence} or null
 */
function matchCamera(cameraString, cameraDatabase, sensorDatabase) {
   if (!cameraString || !cameraDatabase) return null;

   var bestMatch = null;
   var bestConfidence = 0;
   var cameras = cameraDatabase.cameras || cameraDatabase;

   for (var cameraName in cameras) {
      if (!cameras.hasOwnProperty(cameraName)) continue;

      var camera = cameras[cameraName];
      var confidence = calculateMatchConfidence(cameraString, cameraName, camera, true);

      if (confidence > bestConfidence) {
         bestConfidence = confidence;
         bestMatch = {
            name: cameraName,
            camera: camera,
            sensor: null,
            confidence: confidence
         };

         // Look up sensor if available
         if (camera.sensor && sensorDatabase) {
            var sensors = sensorDatabase.sensors || sensorDatabase;
            if (sensors[camera.sensor]) {
               bestMatch.sensor = sensors[camera.sensor];
               bestMatch.sensorId = camera.sensor;
            }
         }
      }

      if (confidence === 1.0) break;
   }

   return bestMatch;
}

/**
 * Match a filter string against the filter database.
 *
 * @param {string} filterString - Filter name from FITS header
 * @param {Object} filterDatabase - Filter database object
 * @returns {Object|null} Match result {filter, confidence} or null
 */
function matchFilter(filterString, filterDatabase) {
   if (!filterString || !filterDatabase) return null;

   var bestMatch = null;
   var bestConfidence = 0;

   // Combine filters and defaults into single lookup
   var allFilters = {};
   var sources = [filterDatabase.filters, filterDatabase.defaults];
   for (var s = 0; s < sources.length; s++) {
      if (sources[s]) {
         for (var k in sources[s]) {
            allFilters[k] = sources[s][k];
         }
      }
   }

   for (var filterName in allFilters) {
      if (!allFilters.hasOwnProperty(filterName)) continue;

      var filter = allFilters[filterName];
      var confidence = calculateMatchConfidence(filterString, filterName, filter, false);

      if (confidence > bestConfidence) {
         bestConfidence = confidence;
         bestMatch = {
            name: filterName,
            filter: filter,
            confidence: confidence
         };
      }

      if (confidence === 1.0) break;
   }

   return bestMatch;
}

/**
 * Get confidence level description.
 *
 * @param {number} confidence - Confidence score 0-1
 * @returns {string} Human-readable confidence level
 */
function getConfidenceLevel(confidence) {
   if (confidence >= 0.95) return "Exact match";
   if (confidence >= 0.85) return "High confidence";
   if (confidence >= 0.7) return "Good match";
   if (confidence >= 0.5) return "Probable match";
   if (confidence > 0) return "Low confidence";
   return "No match";
}

/**
 * Format match result for display.
 *
 * @param {Object} match - Match result object
 * @param {string} type - "camera" or "filter"
 * @returns {string} Formatted string
 */
function formatMatchResult(match, type) {
   if (!match) {
      return type + ": Not identified";
   }

   var confidenceStr = (match.confidence * 100).toFixed(0) + "%";
   var levelStr = getConfidenceLevel(match.confidence);

   var result = type + ": " + match.name + " (" + confidenceStr + " - " + levelStr + ")";

   if (type === "camera" && match.sensorId) {
      result += "\n  Sensor: " + match.sensorId;
   }

   return result;
}
