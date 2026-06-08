/**
 * PhotometricStretch.js
 *
 * Main entry point for the Photometric Stretch Engine.
 * Physics-based RGB stretch with automatic sensor/filter detection.
 *
 * Combines:
 * - RNC-Color-Stretch's proven workflow
 * - Lupton RGB's asinh stretch mathematics
 * - Veralux's sensor-aware QE weighting
 * - Automatic hardware detection from FITS metadata
 */

#feature-id    EZ Stretch BSC > PhotometricStretch
#script-id     PhotometricStretch
#feature-info  Physics-based RGB stretch with automatic sensor/filter detection

// PJSR standard library includes
#include <pjsr/Sizer.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/TextAlign.jsh>
#include <pjsr/DataType.jsh>

// Version check - require PixInsight 1.8.0 or higher
#iflt __PI_VERSION__ 01.08.00
#error This script requires PixInsight 1.8.0 or higher.
#endif

// Local library includes
#include "lib/FITSMetadataParser.js"
#include "lib/HardwareMatcher.js"
#include "lib/RepositoryManager.js"
#include "lib/ResponseCalculator.js"
#include "lib/LuptonStretch.js"
#include "lib/UIComponents.js"

#define VERSION "1.0.23"
#define TITLE "Photometric Stretch Engine"

// Enable automatic garbage collection
var jsAutoGC = true;

/**
 * Main dialog for Photometric Stretch
 */
function PhotometricStretchDialog(view, metadata, cameraMatch, filterMatch, weights, repos) {
   this.__base__ = Dialog;
   this.__base__();

   var dialog = this;

   // Store references
   this.view = view;
   this.metadata = metadata;
   this.cameraMatch = cameraMatch;
   this.filterMatch = filterMatch;
   this.repos = repos;
   this.showPreview = true;

   // Throttling state (per-dialog, not global)
   this.lastPreviewTime = 0;
   this.previewSkipCount = 0;

   // Current parameters
   this.parameters = {
      weights: weights,
      softening: 0.1,
      Q: 1.0,
      blackPoint: 0.0,
      targetBackground: 0.15,
      saturationBoost: 1.0
   };

   // Auto-calculate initial parameters
   var autoParams = autoCalculateParams(view, weights);
   this.parameters.softening = autoParams.softening;
   this.parameters.Q = autoParams.Q;
   this.parameters.blackPoint = autoParams.blackPoint;

   // ========== Detected Hardware Group ==========
   this.hardwareGroup = createGroupBox(this, "Detected Hardware");
   this.hardwareDisplay = createHardwareDisplay(this, metadata, cameraMatch, filterMatch);
   this.hardwareGroup.sizer.add(this.hardwareDisplay.sizer);

   // ========== Stretch Parameters Group ==========
   this.stretchGroup = createGroupBox(this, "Stretch Parameters");

   // Helper to create linear slider with auto button and preview callback
   function createAutoSlider(dlg, label, param, min, max, precision, tooltip, autoParam) {
      var control = createSliderControl(dlg, label, dlg.parameters[param], min, max, precision, tooltip,
         function(value) {
            dlg.parameters[param] = value;
            dlg.schedulePreview();
         });
      var autoBtn = createAutoButton(dlg, "Auto-calculate " + label.toLowerCase(), function() {
         var auto = autoCalculateParams(dialog.view, dialog.parameters.weights);
         control.setValue(auto[autoParam]);
         dialog.parameters[param] = auto[autoParam];
         dialog.schedulePreview();
      });
      var sizer = new HorizontalSizer;
      sizer.add(control.sizer, 100);
      sizer.addSpacing(8);
      sizer.add(autoBtn);
      return { control: control, sizer: sizer };
   }

   // Helper to create logarithmic slider with auto button
   function createAutoLogSlider(dlg, label, param, min, max, precision, tooltip, autoParam) {
      var control = createLogSliderControl(dlg, label, dlg.parameters[param], min, max, precision, tooltip,
         function(value) {
            dlg.parameters[param] = value;
            dlg.schedulePreview();
         });
      var autoBtn = createAutoButton(dlg, "Auto-calculate " + label.toLowerCase(), function() {
         var auto = autoCalculateParams(dialog.view, dialog.parameters.weights);
         control.setValue(auto[autoParam]);
         dialog.parameters[param] = auto[autoParam];
         dialog.schedulePreview();
      });
      var sizer = new HorizontalSizer;
      sizer.add(control.sizer, 100);
      sizer.addSpacing(8);
      sizer.add(autoBtn);
      return { control: control, sizer: sizer };
   }

   // Stretch (α) - Linear amplification factor with logarithmic slider
   // Uses log scale for finer control at low values (1, 3, 5, 10, 20, 50, 100, 200, 500, 1000)
   var stretchRow = createAutoLogSlider(this, "Stretch (α)", "Q", 0.1, 1000.0, 1,
      "Linear amplification factor - logarithmic slider for finer control at low values", "Q");
   this.qControl = stretchRow.control;

   // Q (Softening) - Controls linear/log transition (0-100 range)
   var softeningRow = createAutoSlider(this, "Q (Softening)", "softening", 0.0, 100.0, 1,
      "Controls transition between linear and logarithmic behavior (0=aggressive, 100=gentle)", "softening");
   this.softeningControl = softeningRow.control;

   // Black Point (same range as Lupton RGB plugin: 0-0.01)
   var blackPointRow = createAutoSlider(this, "Black Point", "blackPoint", 0.0, 0.01, 4,
      "Background level to subtract before stretching", "blackPoint");
   this.blackPointControl = blackPointRow.control;

   // Target Background
   this.targetBgControl = createSliderControl(this, "Target Bkg", this.parameters.targetBackground, 0.0, 0.3, 2,
      "Desired output background level", function(value) {
         dialog.parameters.targetBackground = value;
         dialog.schedulePreview();
      });

   // Saturation (same as Lupton RGB plugin: 0.5-2.0)
   this.saturationControl = createSliderControl(this, "Saturation", this.parameters.saturationBoost, 0.5, 2.0, 2,
      "Post-stretch saturation adjustment", function(value) {
         dialog.parameters.saturationBoost = value;
         dialog.schedulePreview();
      });

   this.stretchGroup.sizer.add(stretchRow.sizer);
   this.stretchGroup.sizer.add(softeningRow.sizer);
   this.stretchGroup.sizer.add(blackPointRow.sizer);
   this.stretchGroup.sizer.add(this.targetBgControl.sizer);
   this.stretchGroup.sizer.add(this.saturationControl.sizer);

   // ========== RGB Weights Group ==========
   this.weightsGroup = createGroupBox(this, "RGB Weights");

   // Create weight controls with callback for preview updates
   this.weightControls = createWeightControls(this, weights, function() {
      if (dialog.manualOverride && dialog.manualOverride.checked) {
         dialog.parameters.weights = dialog.weightControls.getWeights();
         dialog.schedulePreview();
      }
   });

   this.recalculateButton = new PushButton(this);
   this.recalculateButton.text = "Recalculate";
   this.recalculateButton.toolTip = "Recalculate weights from sensor/filter data";
   this.recalculateButton.onClick = function() {
      dialog.recalculateWeights();
   };

   this.manualOverride = new CheckBox(this);
   this.manualOverride.text = "Manual override";
   this.manualOverride.checked = false;
   this.manualOverride.toolTip = "Enable manual RGB weight adjustment (disables auto-calculation)";
   this.manualOverride.onCheck = function(checked) {
      dialog.weightControls.setEnabled(checked);
      dialog.recalculateButton.enabled = !checked;
      if (checked) {
         dialog.parameters.weights = dialog.weightControls.getWeights();
         dialog.schedulePreview();
      }
   };
   this.weightControls.setEnabled(false);

   // Layout: Recalculate button on same row as manual override
   var weightButtonSizer = new HorizontalSizer;
   weightButtonSizer.spacing = 8;
   weightButtonSizer.add(this.recalculateButton);
   weightButtonSizer.add(this.manualOverride);
   weightButtonSizer.addStretch();

   this.weightsGroup.sizer.add(this.weightControls.sizer);
   this.weightsGroup.sizer.add(weightButtonSizer);

   // ========== Options Group ==========
   this.optionsGroup = createGroupBox(this, "Options");

   this.showPreviewCheckBox = new CheckBox(this);
   this.showPreviewCheckBox.text = "Show preview";
   this.showPreviewCheckBox.checked = this.showPreview;
   this.showPreviewCheckBox.toolTip = "Show/hide the real-time preview panel";
   this.showPreviewCheckBox.onCheck = function(checked) {
      dialog.showPreview = checked;
      dialog.previewGroupBox.visible = checked;
      if (checked) {
         dialog.schedulePreview();
      }
      dialog.adjustToContents();
   };

   this.optionsGroup.sizer.add(this.showPreviewCheckBox);

   // ========== Presets ==========
   this.presetsLabel = new Label(this);
   this.presetsLabel.text = "Presets:";

   this.presetButtons = createPresetButtons(this, function(preset) {
      dialog.qControl.setValue(preset.Q);
      dialog.softeningControl.setValue(preset.softening);
      dialog.targetBgControl.setValue(preset.targetBackground);
      dialog.saturationControl.setValue(preset.saturationBoost);
      dialog.parameters.Q = preset.Q;
      dialog.parameters.softening = preset.softening;
      dialog.parameters.targetBackground = preset.targetBackground;
      dialog.parameters.saturationBoost = preset.saturationBoost;
      dialog.schedulePreview();
   });

   var presetsSizer = new HorizontalSizer;
   presetsSizer.spacing = 8;
   presetsSizer.add(this.presetsLabel);
   presetsSizer.add(this.presetButtons, 100);

   // ========== Button Bar ==========
   this.buttonBar = createButtonBar(this, {
      onNewInstance: function() {
         dialog.exportParameters();
      },
      onPreview: function() {
         dialog.updateParameters();
         previewStretch(dialog.view, dialog.parameters);
      },
      onApply: function() {
         dialog.updateParameters();
         applyLuptonStretch(dialog.view, dialog.parameters);
      },
      onCancel: function() {
         // Nothing special needed
      }
   });

   // ========== Preview Panel (right side) ==========
   this.previewGroupBox = createGroupBox(this, "Preview");

   // Preview control
   this.previewControl = new PreviewControl(this);
   this.previewControl.setMinSize(350, 280);
   this.previewControl.sourceWindow = view.window;
   this.previewControl.setParameters(this.parameters);

   // Preview mode radio buttons
   this.previewModeLabel = new Label(this);
   this.previewModeLabel.text = "View:";

   this.beforeButton = new RadioButton(this);
   this.beforeButton.text = "Before";
   this.beforeButton.checked = false;
   this.beforeButton.onCheck = function(checked) {
      if (checked) dialog.previewControl.setPreviewMode(1);
   };

   this.splitButton = new RadioButton(this);
   this.splitButton.text = "Split";
   this.splitButton.checked = true;
   this.splitButton.onCheck = function(checked) {
      if (checked) dialog.previewControl.setPreviewMode(2);
   };

   this.afterButton = new RadioButton(this);
   this.afterButton.text = "After";
   this.afterButton.checked = false;
   this.afterButton.onCheck = function(checked) {
      if (checked) dialog.previewControl.setPreviewMode(0);
   };

   var previewModeSizer = new HorizontalSizer;
   previewModeSizer.spacing = 8;
   previewModeSizer.add(this.previewModeLabel);
   previewModeSizer.add(this.beforeButton);
   previewModeSizer.add(this.splitButton);
   previewModeSizer.add(this.afterButton);
   previewModeSizer.addStretch();

   // Set initial preview mode
   this.previewControl.setPreviewMode(2);

   this.previewGroupBox.sizer.add(previewModeSizer);
   this.previewGroupBox.sizer.add(this.previewControl, 100);

   // ========== Main Layout ==========
   // Left panel: controls (no stretch - fixed width)
   var leftPanel = new VerticalSizer;
   leftPanel.spacing = 6;
   leftPanel.add(this.hardwareGroup);
   leftPanel.add(this.stretchGroup);
   leftPanel.add(this.weightsGroup);
   leftPanel.add(this.optionsGroup);
   leftPanel.add(presetsSizer);
   leftPanel.addStretch();
   leftPanel.add(this.buttonBar);

   // Content: left controls + right preview
   var contentSizer = new HorizontalSizer;
   contentSizer.spacing = 8;
   contentSizer.add(leftPanel);  // No stretch factor - stays fixed width
   contentSizer.add(this.previewGroupBox, 100);  // Preview expands

   // Main sizer
   this.sizer = new VerticalSizer;
   this.sizer.margin = 8;
   this.sizer.spacing = 8;
   this.sizer.add(contentSizer, 100);

   this.windowTitle = TITLE;
   this.adjustToContents();

   // Initial preview update
   this.schedulePreview();
}

PhotometricStretchDialog.prototype = new Dialog;

/**
 * Update parameters from UI controls
 */
PhotometricStretchDialog.prototype.updateParameters = function() {
   this.parameters.Q = this.qControl.getValue();
   this.parameters.softening = this.softeningControl.getValue();
   this.parameters.blackPoint = this.blackPointControl.getValue();
   this.parameters.targetBackground = this.targetBgControl.getValue();
   this.parameters.saturationBoost = this.saturationControl.getValue();

   if (this.manualOverride.checked) {
      this.parameters.weights = this.weightControls.getWeights();
   }
};

/**
 * Schedule a preview update with throttling.
 * Uses per-dialog throttle state for responsive sliders.
 */
PhotometricStretchDialog.prototype.schedulePreview = function() {
   if (!this.showPreview) return;
   if (!this.previewControl) return;

   // Throttle updates - skip if last update was less than 80ms ago
   var now = new Date().getTime();
   var elapsed = now - this.lastPreviewTime;

   if (elapsed < 80) {
      this.previewSkipCount++;
      // Force update every 4th skip to keep preview responsive
      if (this.previewSkipCount < 4) {
         return;
      }
   }

   this.previewSkipCount = 0;
   this.lastPreviewTime = now;

   // Update preview
   this.previewControl.setParameters(this.parameters);
   this.previewControl.updatePreview();
};

/**
 * Recalculate weights from sensor/filter data
 */
PhotometricStretchDialog.prototype.recalculateWeights = function() {
   var sensor = this.cameraMatch ? this.cameraMatch.sensor : null;
   var filter = this.filterMatch ? this.filterMatch.filter : null;
   var isMono = this.cameraMatch && this.cameraMatch.camera &&
                this.cameraMatch.camera.type === "Mono";

   var newWeights = calculateRGBWeights(sensor, filter, isMono);
   this.parameters.weights = newWeights;
   this.weightControls.setWeights(newWeights);

   console.writeln("Recalculated weights: " + formatWeights(newWeights));
};

/**
 * Export parameters for process icon
 */
PhotometricStretchDialog.prototype.exportParameters = function() {
   this.updateParameters();
   // In a full implementation, this would create a process icon
   console.writeln("Parameters for new instance:");
   console.writeln("  Q: " + this.parameters.Q);
   console.writeln("  softening: " + this.parameters.softening);
   console.writeln("  blackPoint: " + this.parameters.blackPoint);
   console.writeln("  targetBackground: " + this.parameters.targetBackground);
   console.writeln("  saturationBoost: " + this.parameters.saturationBoost);
   console.writeln("  weights: " + formatWeights(this.parameters.weights));
};

/**
 * Main function
 */
function main() {
   console.writeln("");
   console.writeln("======================================");
   console.writeln(TITLE + " v" + VERSION);
   console.writeln("======================================");

   // Check for active image
   if (!ImageWindow.activeWindow || ImageWindow.activeWindow.isNull) {
      showError("No active image window.\n\nPlease open an RGB image before running this script.");
      return;
   }

   var view = ImageWindow.activeWindow.currentView;
   var image = view.image;

   console.writeln("Image: " + view.id);
   console.writeln("Dimensions: " + image.width + " x " + image.height);
   console.writeln("Channels: " + image.numberOfChannels);

   // Check for RGB image
   if (image.numberOfChannels < 3) {
      showError("This script requires an RGB image (3 channels).\n\nCurrent image has " + image.numberOfChannels + " channel(s).");
      return;
   }

   // Load repositories - get script directory from current file path
   var scriptPath = File.extractDrive(#__FILE__) + File.extractDirectory(#__FILE__);
   var dataDir = scriptPath + "/data";
   console.writeln("Loading data from: " + dataDir);

   var repos = loadRepositories(dataDir);

   if (!repos.sensors || !repos.cameras || !repos.filters) {
      var missing = [];
      if (!repos.sensors) missing.push("sensors.json");
      if (!repos.cameras) missing.push("cameras.json");
      if (!repos.filters) missing.push("filters.json");
      showWarning("Failed to load: " + missing.join(", ") + "\n\n" +
         "Camera and filter detection will be limited.\n" +
         "RGB weights will use generic Rec.709 values.\n\n" +
         "Check the data folder: " + dataDir);
   }

   // Parse FITS metadata
   console.writeln("");
   console.writeln("Parsing FITS metadata...");
   var metadata = parseFITSMetadata(view);
   logMetadata(metadata);

   // Match hardware
   console.writeln("");
   console.writeln("Matching hardware...");

   var cameraMatch = null;
   var filterMatch = null;

   if (metadata.camera) {
      cameraMatch = matchCamera(metadata.camera, repos.cameras, repos.sensors);
      if (cameraMatch && cameraMatch.camera) {
         console.writeln(formatMatchResult(cameraMatch, "Camera"));
      } else {
         console.warningln("Camera '" + metadata.camera + "' not found in database - using default weights");
         cameraMatch = null;
      }
   } else {
      console.writeln("No camera info in FITS headers");
   }

   if (metadata.filter) {
      filterMatch = matchFilter(metadata.filter, repos.filters);
      if (filterMatch && filterMatch.filter) {
         console.writeln(formatMatchResult(filterMatch, "Filter"));
      } else {
         console.warningln("Filter '" + metadata.filter + "' not found in database");
         filterMatch = null;
      }
   } else {
      console.writeln("No filter info in FITS headers");
   }

   // Calculate initial weights
   console.writeln("");
   console.writeln("Calculating RGB weights...");

   var sensor = null;
   var filter = null;
   var isMono = false;

   if (cameraMatch && cameraMatch.camera && cameraMatch.sensor) {
      sensor = cameraMatch.sensor;
      isMono = cameraMatch.camera.type === "Mono";
      console.writeln("  Using sensor: " + cameraMatch.sensorId);
   } else if (cameraMatch && cameraMatch.camera && !cameraMatch.sensor) {
      console.warningln("  Camera matched but no sensor data - using defaults");
   }

   if (filterMatch && filterMatch.filter) {
      filter = filterMatch.filter;
      console.writeln("  Using filter: " + filterMatch.name);
   }

   var weights = calculateRGBWeights(sensor, filter, isMono);
   console.writeln("  Weights: " + formatWeights(weights));

   // Show dialog
   console.writeln("");
   console.writeln("Opening dialog...");

   var dialog = new PhotometricStretchDialog(view, metadata, cameraMatch, filterMatch, weights, repos);

   if (dialog.execute()) {
      console.writeln("");
      console.writeln("Stretch applied successfully!");
   } else {
      console.writeln("");
      console.writeln("Cancelled by user.");
   }

   console.writeln("");
   console.writeln("======================================");
}

main();
