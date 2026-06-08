/**
 * RepositoryManager.js
 *
 * Load JSON databases and provide query interface for sensors, cameras, and filters.
 *
 * Note: Requires #include <pjsr/DataType.jsh> in the main script.
 */

// Module-level cache for loaded repositories
var _repositories = {
   sensors: null,
   cameras: null,
   filters: null,
   loaded: false,
   basePath: ""
};

/**
 * Unwrap a repository object to get the actual collection.
 * Handles both {collection: {...}} and flat {...} formats.
 *
 * @param {Object} repo - Repository object
 * @param {string} key - Collection key (e.g., "sensors", "cameras", "filters")
 * @returns {Object} The collection object
 */
function unwrap(repo, key) {
   if (!repo) return {};
   return repo[key] || repo;
}

/**
 * Load a JSON file from disk.
 *
 * @param {string} path - Full path to JSON file
 * @returns {Object|null} Parsed JSON or null on error
 */
function loadJSONFile(path) {
   var file = null;
   try {
      file = new File();
      // Skip File.exists check - directly try to open (more robust on Windows)
      file.openForReading(path);
      var text = file.read(DataType_ByteArray, file.size);
      file.close();
      return JSON.parse(text.toString());
   } catch (e) {
      // Check if it's a file-not-found error
      var msg = e.message || "";
      if (msg.indexOf("no such file") !== -1 || msg.indexOf("cannot open") !== -1 ||
          msg.indexOf("not found") !== -1 || msg.indexOf("does not exist") !== -1) {
         console.warningln("File not found: " + path);
      } else if (msg.indexOf("JSON") !== -1 || msg.indexOf("parse") !== -1 ||
          msg.indexOf("syntax") !== -1 || msg.indexOf("Unexpected") !== -1) {
         console.criticalln("Failed to load database: " + path);
         console.criticalln("  JSON syntax error: " + msg);
         console.criticalln("  Please verify the file contains valid JSON.");
      } else {
         console.criticalln("Failed to load database: " + path);
         console.criticalln("  " + msg);
      }
      // Ensure file is closed on error
      if (file && file.isOpen) {
         try { file.close(); } catch (closeErr) {}
      }
      return null;
   }
}

/**
 * Load all repositories from the data directory.
 *
 * @param {string} basePath - Path to data directory (e.g., script_dir + "/data")
 * @returns {Object} Loaded repositories {sensors, cameras, filters}
 */
function loadRepositories(basePath) {
   console.writeln("Loading photometric repositories from: " + basePath);

   _repositories.basePath = basePath;

   // Load sensors
   var sensorsPath = basePath + "/sensors.json";
   _repositories.sensors = loadJSONFile(sensorsPath);
   if (_repositories.sensors) {
      var sensorCount = Object.keys(_repositories.sensors.sensors || _repositories.sensors).length;
      console.writeln("  Loaded " + sensorCount + " sensors");
   }

   // Load cameras
   var camerasPath = basePath + "/cameras.json";
   _repositories.cameras = loadJSONFile(camerasPath);
   if (_repositories.cameras) {
      var cameraCount = Object.keys(_repositories.cameras.cameras || _repositories.cameras).length;
      console.writeln("  Loaded " + cameraCount + " cameras");
   }

   // Load filters
   var filtersPath = basePath + "/filters.json";
   _repositories.filters = loadJSONFile(filtersPath);
   if (_repositories.filters) {
      var filterCount = Object.keys(_repositories.filters.filters || {}).length;
      var defaultCount = Object.keys(_repositories.filters.defaults || {}).length;
      console.writeln("  Loaded " + filterCount + " filters + " + defaultCount + " defaults");
   }

   _repositories.loaded = true;

   return {
      sensors: _repositories.sensors,
      cameras: _repositories.cameras,
      filters: _repositories.filters
   };
}

/**
 * Check if repositories are loaded.
 *
 * @returns {boolean} True if loaded
 */
function repositoriesLoaded() {
   return _repositories.loaded;
}

/**
 * Get cached repositories.
 *
 * @returns {Object} Cached repositories
 */
function getRepositories() {
   return {
      sensors: _repositories.sensors,
      cameras: _repositories.cameras,
      filters: _repositories.filters
   };
}

/**
 * Get a sensor by ID.
 *
 * @param {string} sensorId - Sensor ID (e.g., "IMX571")
 * @returns {Object|null} Sensor data or null
 */
function getSensor(sensorId) {
   return unwrap(_repositories.sensors, "sensors")[sensorId] || null;
}

/**
 * Get a camera by exact name.
 *
 * @param {string} cameraName - Camera name
 * @returns {Object|null} Camera data or null
 */
function getCamera(cameraName) {
   return unwrap(_repositories.cameras, "cameras")[cameraName] || null;
}

/**
 * Get a filter by exact name.
 *
 * @param {string} filterName - Filter name
 * @returns {Object|null} Filter data or null
 */
function getFilter(filterName) {
   if (!_repositories.filters) return null;
   var filters = unwrap(_repositories.filters, "filters");
   return filters[filterName] || (_repositories.filters.defaults || {})[filterName] || null;
}

/**
 * Find a camera using hardware matching.
 *
 * @param {string} cameraString - Camera string from FITS header
 * @returns {Object|null} Match result from HardwareMatcher
 */
function findCamera(cameraString) {
   if (!_repositories.cameras) return null;

   // Use HardwareMatcher if available
   if (typeof matchCamera === "function") {
      return matchCamera(cameraString, _repositories.cameras, _repositories.sensors);
   }

   // Fallback to simple lookup
   return {
      name: cameraString,
      camera: getCamera(cameraString),
      sensor: null,
      confidence: getCamera(cameraString) ? 1.0 : 0
   };
}

/**
 * Find a filter using hardware matching.
 *
 * @param {string} filterString - Filter string from FITS header
 * @returns {Object|null} Match result from HardwareMatcher
 */
function findFilter(filterString) {
   if (!_repositories.filters) return null;

   // Use HardwareMatcher if available
   if (typeof matchFilter === "function") {
      return matchFilter(filterString, _repositories.filters);
   }

   // Fallback to simple lookup
   return {
      name: filterString,
      filter: getFilter(filterString),
      confidence: getFilter(filterString) ? 1.0 : 0
   };
}

/**
 * Get default RGB weights (Rec.709).
 *
 * @returns {Object} Default weights {R, G, B}
 */
function getDefaultWeights() {
   return {
      R: 0.2126,
      G: 0.7152,
      B: 0.0722
   };
}

/**
 * Get list of all camera names in database.
 *
 * @returns {string[]} Array of camera names
 */
function listCameras() {
   return Object.keys(unwrap(_repositories.cameras, "cameras")).sort();
}

/**
 * Get list of all filter names in database.
 *
 * @returns {string[]} Array of filter names
 */
function listFilters() {
   if (!_repositories.filters) return [];
   var filters = Object.keys(unwrap(_repositories.filters, "filters"));
   var defaults = Object.keys(_repositories.filters.defaults || {});
   return filters.concat(defaults).sort();
}

/**
 * Get list of all sensor IDs in database.
 *
 * @returns {string[]} Array of sensor IDs
 */
function listSensors() {
   return Object.keys(unwrap(_repositories.sensors, "sensors")).sort();
}

/**
 * Reload repositories from disk.
 *
 * @returns {Object} Reloaded repositories
 */
function reloadRepositories() {
   if (!_repositories.basePath) {
      console.warningln("Cannot reload: no base path set");
      return null;
   }

   _repositories.sensors = null;
   _repositories.cameras = null;
   _repositories.filters = null;
   _repositories.loaded = false;

   return loadRepositories(_repositories.basePath);
}

/**
 * Get repository version info.
 *
 * @returns {Object} Version info for each repository
 */
function getRepositoryVersions() {
   var versions = {
      sensors: "unknown",
      cameras: "unknown",
      filters: "unknown"
   };

   if (_repositories.sensors && _repositories.sensors.version) {
      versions.sensors = _repositories.sensors.version;
   }
   if (_repositories.cameras && _repositories.cameras.version) {
      versions.cameras = _repositories.cameras.version;
   }
   if (_repositories.filters && _repositories.filters.version) {
      versions.filters = _repositories.filters.version;
   }

   return versions;
}
