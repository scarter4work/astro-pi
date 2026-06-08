/**
 * UIComponents.js
 *
 * Dialog and control helpers for the Photometric Stretch Engine UI.
 * Provides reusable components for building the PixInsight dialog.
 *
 * Note: This file requires PJSR standard includes from the main script:
 *   #include <pjsr/Sizer.jsh>
 *   #include <pjsr/NumericControl.jsh>
 *   #include <pjsr/StdButton.jsh>
 *   #include <pjsr/StdIcon.jsh>
 *   #include <pjsr/TextAlign.jsh>
 */

/**
 * Script version and info
 */
var ScriptInfo = {
   name: "Photometric Stretch Engine",
   version: "1.0.0",
   author: "Photometric Stretch Project",
   description: "Physics-based RGB stretch with automatic sensor/filter detection"
};

// ============================================================================
// Preview Throttling Mechanism
// ============================================================================

/**
 * Global preview throttle state
 */
var _previewThrottle = {
   lastUpdateTime: 0,
   skipCount: 0,
   throttleInterval: 80,  // milliseconds between updates
   maxSkips: 4            // force update after this many skips
};

/**
 * Schedule a throttled preview update.
 * Prevents excessive updates while keeping the UI responsive.
 *
 * @param {Function} updateFn - The function to call for preview update
 * @returns {boolean} True if update was executed, false if skipped
 */
function schedulePreviewUpdate(updateFn) {
   if (!updateFn) return false;

   var now = new Date().getTime();
   var elapsed = now - _previewThrottle.lastUpdateTime;

   if (elapsed < _previewThrottle.throttleInterval) {
      _previewThrottle.skipCount++;
      // Force update every Nth skip to keep preview somewhat responsive
      if (_previewThrottle.skipCount < _previewThrottle.maxSkips) {
         return false;
      }
   }

   _previewThrottle.skipCount = 0;
   _previewThrottle.lastUpdateTime = now;

   updateFn();
   return true;
}

// ============================================================================
// Preview Control
// ============================================================================

/**
 * PreviewControl - Real-time preview window using VectorGraphics.
 * Based on RNC-ColorStretch preview pattern.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {Function} stretchEngine - Function(r,g,b,params) that returns [r,g,b] stretched
 */
function PreviewControl(parent, stretchEngine) {
   this.__base__ = Frame;
   this.__base__(parent);

   var self = this;
   this.stretchEngine = stretchEngine;
   this.scaledImage = null;
   this.sourceWindow = null;
   this.parameters = null;
   this.previewMode = 0;     // 0=After, 1=Before, 2=Split
   this.splitPosition = 50;  // Split position (0-100%)

   // ScrollBox for viewport
   this.scrollbox = new ScrollBox(this);
   this.scrollbox.autoScroll = true;
   this.scrollbox.tracking = true;

   // Viewport paint handler
   this.scrollbox.viewport.onPaint = function(x0, y0, x1, y1) {
      var graphics = new VectorGraphics(this);
      graphics.fillRect(x0, y0, x1, y1, new Brush(0xff202020));

      if (self.scaledImage) {
         var offsetX = Math.max(0, (this.width - self.scaledImage.width) / 2);
         var offsetY = Math.max(0, (this.height - self.scaledImage.height) / 2);
         graphics.translateTransformation(offsetX, offsetY);
         graphics.drawBitmap(0, 0, self.scaledImage);

         // Draw split line in split mode
         if (self.previewMode === 2) {
            var splitX = self.scaledImage.width * self.splitPosition / 100;
            graphics.pen = new Pen(0xffff0000, 2);
            graphics.drawLine(splitX, 0, splitX, self.scaledImage.height);

            // Labels
            graphics.font = new Font("helvetica", 10);
            graphics.pen = new Pen(0xffffffff);
            graphics.drawText(5, 15, "Before");
            graphics.drawText(splitX + 5, 15, "After");
         }
      } else {
         // No image message
         graphics.font = new Font("helvetica", 12);
         graphics.pen = new Pen(0xff888888);
         var msg = "No preview available";
         graphics.drawText((this.width - 100) / 2, this.height / 2, msg);
      }

      graphics.end();
   };

   // Resize handler - regenerate preview
   this.scrollbox.viewport.onResize = function(wNew, hNew, wOld, hOld) {
      if (wNew > 0 && hNew > 0) {
         self.regenerate();
      }
   };

   // Click handler for split position
   this.scrollbox.viewport.onMousePress = function(x, y, button, modifiers) {
      if (self.previewMode === 2 && self.scaledImage) {
         var offsetX = Math.max(0, (this.width - self.scaledImage.width) / 2);
         var relX = x - offsetX;
         if (relX >= 0 && relX <= self.scaledImage.width) {
            self.splitPosition = Math.round(100 * relX / self.scaledImage.width);
            self.splitPosition = Math.max(10, Math.min(90, self.splitPosition));
            self.regenerate();
         }
      }
   };

   this.setMinSize(200, 150);
   this.sizer = new VerticalSizer;
   this.sizer.add(this.scrollbox, 100);

   /**
    * Set the source image window
    */
   this.setSourceWindow = function(window) {
      this.sourceWindow = window;
      this.regenerate();
   };

   /**
    * Set stretch parameters
    */
   this.setParameters = function(params) {
      this.parameters = params;
   };

   /**
    * Set preview mode (0=After, 1=Before, 2=Split)
    */
   this.setPreviewMode = function(mode) {
      this.previewMode = mode;
      this.regenerate();
   };

   /**
    * Update preview with current parameters
    */
   this.updatePreview = function() {
      this.regenerate();
   };

   /**
    * Regenerate the preview bitmap
    */
   this.regenerate = function() {
      if (!this.sourceWindow || this.sourceWindow.isNull) {
         this.scaledImage = null;
         this.scrollbox.viewport.update();
         return;
      }

      var vpWidth = this.scrollbox.viewport.width;
      var vpHeight = this.scrollbox.viewport.height;
      if (vpWidth <= 0 || vpHeight <= 0) return;

      var image = this.sourceWindow.mainView.image;
      if (!image || image.numberOfChannels < 3) return;

      var imgW = image.width;
      var imgH = image.height;

      // Calculate preview size preserving aspect ratio (fit to viewport)
      var maxW = vpWidth - 10;
      var maxH = vpHeight - 10;
      var scale = Math.min(maxW / imgW, maxH / imgH);
      var outW = Math.max(1, Math.round(imgW * scale));
      var outH = Math.max(1, Math.round(imgH * scale));

      // Generate preview bitmap
      this.scaledImage = this.generatePreviewBitmap(outW, outH);
      this.scrollbox.viewport.update();
   };

   /**
    * Generate a preview bitmap at the specified size
    */
   this.generatePreviewBitmap = function(outWidth, outHeight) {
      if (!this.sourceWindow || !this.parameters) return null;

      var image = this.sourceWindow.mainView.image;
      var imgWidth = image.width;
      var imgHeight = image.height;

      var bitmap = new Bitmap(outWidth, outHeight);
      var scaleX = imgWidth / outWidth;
      var scaleY = imgHeight / outHeight;
      var splitX = outWidth * this.splitPosition / 100;

      var p = this.parameters;
      var weights = p.weights || { R: 0.2126, G: 0.7152, B: 0.0722 };
      var normFactor = 1.0 / Math.asinh(p.Q / p.softening);

      for (var py = 0; py < outHeight; py++) {
         var iy = Math.min(Math.floor(py * scaleY), imgHeight - 1);

         for (var px = 0; px < outWidth; px++) {
            var ix = Math.min(Math.floor(px * scaleX), imgWidth - 1);

            // Sample original pixel
            var r = image.sample(ix, iy, 0);
            var g = image.sample(ix, iy, 1);
            var b = image.sample(ix, iy, 2);

            var rOut, gOut, bOut;

            // Determine if showing before or after
            var showBefore = (this.previewMode === 1) ||
                            (this.previewMode === 2 && px < splitX);

            if (showBefore) {
               // "Before" - apply basic auto-stretch for visibility
               var maxVal = Math.max(r, g, b);
               var boost = maxVal > 0 ? Math.min(10, 0.5 / maxVal) : 1;
               rOut = Math.min(1, r * boost);
               gOut = Math.min(1, g * boost);
               bOut = Math.min(1, b * boost);
            } else {
               // "After" - apply Lupton stretch
               r = Math.max(0, r - p.blackPoint);
               g = Math.max(0, g - p.blackPoint);
               b = Math.max(0, b - p.blackPoint);

               var I = weights.R * r + weights.G * g + weights.B * b;

               if (I < 1e-10) {
                  rOut = 0;
                  gOut = 0;
                  bOut = 0;
               } else {
                  var stretchedI = Math.asinh(p.Q * I / p.softening) * normFactor;
                  var scaleFactor = stretchedI / I;

                  rOut = r * scaleFactor;
                  gOut = g * scaleFactor;
                  bOut = b * scaleFactor;

                  // Saturation boost
                  if (p.saturationBoost !== 1.0) {
                     var lum = weights.R * rOut + weights.G * gOut + weights.B * bOut;
                     rOut = lum + p.saturationBoost * (rOut - lum);
                     gOut = lum + p.saturationBoost * (gOut - lum);
                     bOut = lum + p.saturationBoost * (bOut - lum);
                  }

                  // Target background shift
                  if (p.targetBackground > 0) {
                     var bgShift = p.targetBackground * (1.0 - stretchedI);
                     rOut += bgShift;
                     gOut += bgShift;
                     bOut += bgShift;
                  }
               }
            }

            // Clip and convert to 8-bit
            var r8 = Math.max(0, Math.min(255, Math.round(rOut * 255)));
            var g8 = Math.max(0, Math.min(255, Math.round(gOut * 255)));
            var b8 = Math.max(0, Math.min(255, Math.round(bOut * 255)));

            bitmap.setPixel(px, py, 0xff000000 | (r8 << 16) | (g8 << 8) | b8);
         }
      }

      return bitmap;
   };
}

PreviewControl.prototype = new Frame;

/**
 * Create a labeled slider with numeric edit control.
 * Supports optional throttled callback for real-time preview updates.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {string} label - Label text
 * @param {number} value - Initial value
 * @param {number} min - Minimum value
 * @param {number} max - Maximum value
 * @param {number} precision - Decimal places
 * @param {string} tooltip - Tooltip text
 * @param {Function} onValueChanged - Optional callback(value) called on change (throttled)
 * @returns {Object} {sizer, slider, edit, setValue, getValue, setOnValueChanged}
 */
function createSliderControl(parent, label, value, min, max, precision, tooltip, onValueChanged) {
   precision = precision || 2;

   var labelControl = new Label(parent);
   labelControl.text = label + ":";
   labelControl.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   labelControl.minWidth = 100;

   var slider = new Slider(parent);
   slider.minValue = 0;
   slider.maxValue = 1000;
   slider.value = Math.round((value - min) / (max - min) * 1000);
   slider.toolTip = tooltip || "";

   var edit = new NumericEdit(parent);
   edit.real = true;
   edit.setPrecision(precision);
   edit.setRange(min, max);
   edit.setValue(value);
   edit.toolTip = tooltip || "";
   edit.minWidth = 70;

   // Store callback reference
   var valueChangedCallback = onValueChanged || null;

   // Notify callback with throttling
   var notifyChange = function(newValue) {
      if (valueChangedCallback) {
         schedulePreviewUpdate(function() {
            valueChangedCallback(newValue);
         });
      }
   };

   // Sync slider to edit
   slider.onValueUpdated = function(sliderValue) {
      var realValue = min + (sliderValue / 1000) * (max - min);
      edit.setValue(realValue);
      notifyChange(realValue);
   };

   // Sync edit to slider
   edit.onValueUpdated = function(editValue) {
      var sliderValue = Math.round((editValue - min) / (max - min) * 1000);
      slider.value = sliderValue;
      notifyChange(editValue);
   };

   var sizer = new HorizontalSizer;
   sizer.spacing = 6;
   sizer.add(labelControl);
   sizer.add(slider, 100);
   sizer.add(edit);

   return {
      sizer: sizer,
      slider: slider,
      edit: edit,
      label: labelControl,
      setValue: function(v) {
         edit.setValue(v);
         slider.value = Math.round((v - min) / (max - min) * 1000);
      },
      getValue: function() {
         return edit.value;
      },
      setOnValueChanged: function(callback) {
         valueChangedCallback = callback;
      }
   };
}

/**
 * Create a logarithmic slider for values that need finer control at low end.
 * Slider position maps logarithmically: small steps at low values, large steps at high.
 * Good for Stretch (α) parameter: 1, 3, 5, 10, 20, 50, 100, 200, 500, 1000
 *
 * @param {Dialog} parent - Parent dialog
 * @param {string} label - Label text
 * @param {number} value - Initial value
 * @param {number} min - Minimum value (must be > 0)
 * @param {number} max - Maximum value
 * @param {number} precision - Decimal places
 * @param {string} tooltip - Tooltip text
 * @param {Function} onValueChanged - Optional callback
 * @returns {Object} Same interface as createSliderControl
 */
function createLogSliderControl(parent, label, value, min, max, precision, tooltip, onValueChanged) {
   precision = precision || 1;
   min = Math.max(0.1, min);  // Ensure min > 0 for log

   var labelControl = new Label(parent);
   labelControl.text = label + ":";
   labelControl.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   labelControl.minWidth = 100;

   // Log scale conversion functions
   var logMin = Math.log10(min);
   var logMax = Math.log10(max);
   var logRange = logMax - logMin;

   function valueToSlider(v) {
      v = Math.max(min, Math.min(max, v));
      return Math.round((Math.log10(v) - logMin) / logRange * 1000);
   }

   function sliderToValue(s) {
      var logVal = logMin + (s / 1000) * logRange;
      return Math.pow(10, logVal);
   }

   var slider = new Slider(parent);
   slider.minValue = 0;
   slider.maxValue = 1000;
   slider.value = valueToSlider(value);
   slider.toolTip = tooltip || "";

   var edit = new NumericEdit(parent);
   edit.real = true;
   edit.setPrecision(precision);
   edit.setRange(min, max);
   edit.setValue(value);
   edit.toolTip = tooltip || "";
   edit.minWidth = 70;

   var valueChangedCallback = onValueChanged || null;

   var notifyChange = function(newValue) {
      if (valueChangedCallback) {
         schedulePreviewUpdate(function() {
            valueChangedCallback(newValue);
         });
      }
   };

   slider.onValueUpdated = function(sliderValue) {
      var realValue = sliderToValue(sliderValue);
      edit.setValue(realValue);
      notifyChange(realValue);
   };

   edit.onValueUpdated = function(editValue) {
      slider.value = valueToSlider(editValue);
      notifyChange(editValue);
   };

   var sizer = new HorizontalSizer;
   sizer.spacing = 6;
   sizer.add(labelControl);
   sizer.add(slider, 100);
   sizer.add(edit);

   return {
      sizer: sizer,
      slider: slider,
      edit: edit,
      label: labelControl,
      setValue: function(v) {
         edit.setValue(v);
         slider.value = valueToSlider(v);
      },
      getValue: function() {
         return edit.value;
      },
      setOnValueChanged: function(callback) {
         valueChangedCallback = callback;
      }
   };
}

/**
 * Create an Auto button for a control.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {string} tooltip - Tooltip text
 * @param {Function} onClick - Click handler
 * @returns {PushButton} The button
 */
function createAutoButton(parent, tooltip, onClick) {
   var button = new PushButton(parent);
   button.text = "Auto";
   button.toolTip = tooltip || "Auto-calculate this parameter";
   button.onClick = onClick || function() {};
   return button;
}

/**
 * Create a group box with title.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {string} title - Group title
 * @returns {GroupBox} The group box
 */
function createGroupBox(parent, title) {
   var group = new GroupBox(parent);
   group.title = title;
   group.sizer = new VerticalSizer;
   group.sizer.margin = 8;
   group.sizer.spacing = 6;
   return group;
}

/**
 * Create a combo box with items.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {string[]} items - List of items
 * @param {number} selectedIndex - Initially selected index
 * @returns {ComboBox} The combo box
 */
function createComboBox(parent, items, selectedIndex) {
   var combo = new ComboBox(parent);
   for (var i = 0; i < items.length; i++) {
      combo.addItem(items[i]);
   }
   combo.currentItem = selectedIndex || 0;
   return combo;
}

/**
 * Create a status indicator label.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {string} text - Initial text
 * @param {boolean} isGood - True for green, false for yellow/warning
 * @returns {Label} The label
 */
function createStatusLabel(parent, text, isGood) {
   var label = new Label(parent);
   label.text = text;
   if (isGood) {
      label.textColor = 0xFF008800;  // Green
   } else {
      label.textColor = 0xFFCC8800;  // Orange/warning
   }
   return label;
}

/**
 * Create button bar with standard buttons.
 *
 * @param {Dialog} dialog - Parent dialog
 * @param {Object} handlers - Handler functions {onNewInstance, onPreview, onApply, onCancel}
 * @returns {HorizontalSizer} Sizer containing buttons
 */
function createButtonBar(dialog, handlers) {
   handlers = handlers || {};

   var newInstanceButton = new ToolButton(dialog);
   newInstanceButton.icon = dialog.scaledResource(":/process-interface/new-instance.png");
   newInstanceButton.setScaledFixedSize(24, 24);
   newInstanceButton.toolTip = "New Instance";
   newInstanceButton.onMousePress = function() {
      if (handlers.onNewInstance) {
         handlers.onNewInstance();
      }
   };

   var previewButton = new PushButton(dialog);
   previewButton.text = "Preview";
   previewButton.toolTip = "Preview stretch using STF (non-destructive)";
   previewButton.onClick = function() {
      if (handlers.onPreview) {
         handlers.onPreview();
      }
   };

   var applyButton = new PushButton(dialog);
   applyButton.text = "Apply";
   applyButton.toolTip = "Apply stretch to image";
   applyButton.onClick = function() {
      if (handlers.onApply) {
         handlers.onApply();
      }
      dialog.ok();
   };

   var cancelButton = new PushButton(dialog);
   cancelButton.text = "Cancel";
   cancelButton.onClick = function() {
      if (handlers.onCancel) {
         handlers.onCancel();
      }
      dialog.cancel();
   };

   var sizer = new HorizontalSizer;
   sizer.spacing = 8;
   sizer.add(newInstanceButton);
   sizer.addStretch();
   sizer.add(previewButton);
   sizer.add(applyButton);
   sizer.add(cancelButton);

   return sizer;
}

/**
 * Create preset buttons.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {Function} onPresetSelect - Handler(presetName)
 * @returns {HorizontalSizer} Sizer containing preset buttons
 */
function createPresetButtons(parent, onPresetSelect) {
   // Presets with updated parameter ranges:
   // Q (Stretch α): 0.1-1000 (log scale), softening (Q): 0-100
   var presets = [
      { name: "Default", Q: 100.0, softening: 25.0, targetBackground: 0.15, saturationBoost: 1.0 },
      { name: "Galaxy", Q: 200.0, softening: 30.0, targetBackground: 0.12, saturationBoost: 1.1 },
      { name: "Nebula", Q: 300.0, softening: 18.0, targetBackground: 0.10, saturationBoost: 1.2 },
      { name: "Starfield", Q: 80.0, softening: 35.0, targetBackground: 0.18, saturationBoost: 0.9 }
   ];

   var sizer = new HorizontalSizer;
   sizer.spacing = 6;

   for (var i = 0; i < presets.length; i++) {
      (function(preset) {
         var button = new PushButton(parent);
         button.text = preset.name;
         button.onClick = function() {
            if (onPresetSelect) {
               onPresetSelect(preset);
            }
         };
         sizer.add(button);
      })(presets[i]);
   }

   sizer.addStretch();

   return sizer;
}

/**
 * Create weight slider controls.
 * Uses 1-100 scale for intuitive adjustment, internally normalized to sum to 1.0.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {Object} weights - Initial weights {R, G, B} (0-1 normalized)
 * @param {Function} onValueChanged - Optional callback when weights change
 * @returns {Object} {sizer, setWeights, getWeights, setEnabled}
 */
function createWeightControls(parent, weights, onValueChanged) {
   weights = weights || { R: 0.333, G: 0.334, B: 0.333 };

   // Convert 0-1 weights to 1-100 scale
   var total = weights.R + weights.G + weights.B;
   var scale = 100 / total;
   var rValue = Math.round(weights.R * scale);
   var gValue = Math.round(weights.G * scale);
   var bValue = Math.round(weights.B * scale);

   // R weight
   var rLabel = new Label(parent);
   rLabel.text = "R:";
   rLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   rLabel.minWidth = 20;

   var rSlider = new Slider(parent);
   rSlider.minValue = 1;
   rSlider.maxValue = 100;
   rSlider.value = rValue;
   rSlider.toolTip = "Red channel weight (1-100)";

   var rEdit = new NumericEdit(parent);
   rEdit.real = false;
   rEdit.setRange(1, 100);
   rEdit.setValue(rValue);
   rEdit.minWidth = 50;

   // G weight
   var gLabel = new Label(parent);
   gLabel.text = "G:";
   gLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   gLabel.minWidth = 20;

   var gSlider = new Slider(parent);
   gSlider.minValue = 1;
   gSlider.maxValue = 100;
   gSlider.value = gValue;
   gSlider.toolTip = "Green channel weight (1-100)";

   var gEdit = new NumericEdit(parent);
   gEdit.real = false;
   gEdit.setRange(1, 100);
   gEdit.setValue(gValue);
   gEdit.minWidth = 50;

   // B weight
   var bLabel = new Label(parent);
   bLabel.text = "B:";
   bLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   bLabel.minWidth = 20;

   var bSlider = new Slider(parent);
   bSlider.minValue = 1;
   bSlider.maxValue = 100;
   bSlider.value = bValue;
   bSlider.toolTip = "Blue channel weight (1-100)";

   var bEdit = new NumericEdit(parent);
   bEdit.real = false;
   bEdit.setRange(1, 100);
   bEdit.setValue(bValue);
   bEdit.minWidth = 50;

   // Sync slider/edit pairs
   rSlider.onValueUpdated = function(v) {
      rEdit.setValue(v);
      if (onValueChanged) onValueChanged();
   };
   rEdit.onValueUpdated = function(v) {
      rSlider.value = Math.round(v);
      if (onValueChanged) onValueChanged();
   };

   gSlider.onValueUpdated = function(v) {
      gEdit.setValue(v);
      if (onValueChanged) onValueChanged();
   };
   gEdit.onValueUpdated = function(v) {
      gSlider.value = Math.round(v);
      if (onValueChanged) onValueChanged();
   };

   bSlider.onValueUpdated = function(v) {
      bEdit.setValue(v);
      if (onValueChanged) onValueChanged();
   };
   bEdit.onValueUpdated = function(v) {
      bSlider.value = Math.round(v);
      if (onValueChanged) onValueChanged();
   };

   // Layout: R row
   var rSizer = new HorizontalSizer;
   rSizer.spacing = 4;
   rSizer.add(rLabel);
   rSizer.add(rSlider, 100);
   rSizer.add(rEdit);

   // Layout: G row
   var gSizer = new HorizontalSizer;
   gSizer.spacing = 4;
   gSizer.add(gLabel);
   gSizer.add(gSlider, 100);
   gSizer.add(gEdit);

   // Layout: B row
   var bSizer = new HorizontalSizer;
   bSizer.spacing = 4;
   bSizer.add(bLabel);
   bSizer.add(bSlider, 100);
   bSizer.add(bEdit);

   // Main vertical sizer
   var sizer = new VerticalSizer;
   sizer.spacing = 4;
   sizer.add(rSizer);
   sizer.add(gSizer);
   sizer.add(bSizer);

   return {
      sizer: sizer,
      rSlider: rSlider,
      gSlider: gSlider,
      bSlider: bSlider,
      rEdit: rEdit,
      gEdit: gEdit,
      bEdit: bEdit,
      setWeights: function(w) {
         // Convert 0-1 weights to 1-100 scale
         var t = w.R + w.G + w.B;
         var s = 100 / t;
         rEdit.setValue(Math.round(w.R * s));
         gEdit.setValue(Math.round(w.G * s));
         bEdit.setValue(Math.round(w.B * s));
         rSlider.value = Math.round(w.R * s);
         gSlider.value = Math.round(w.G * s);
         bSlider.value = Math.round(w.B * s);
      },
      getWeights: function() {
         // Convert 1-100 values to normalized 0-1 weights
         var r = rEdit.value;
         var g = gEdit.value;
         var b = bEdit.value;
         var total = r + g + b;
         if (total > 0) {
            return { R: r / total, G: g / total, B: b / total };
         }
         return { R: 0.333, G: 0.334, B: 0.333 };
      },
      setEnabled: function(enabled) {
         rSlider.enabled = enabled;
         gSlider.enabled = enabled;
         bSlider.enabled = enabled;
         rEdit.enabled = enabled;
         gEdit.enabled = enabled;
         bEdit.enabled = enabled;
      }
   };
}

/**
 * Create hardware detection display.
 *
 * @param {Dialog} parent - Parent dialog
 * @param {Object} metadata - FITS metadata
 * @param {Object} cameraMatch - Camera match result
 * @param {Object} filterMatch - Filter match result
 * @returns {Object} {sizer, updateCamera, updateFilter, updateStatus}
 */
function createHardwareDisplay(parent, metadata, cameraMatch, filterMatch) {
   var cameraLabel = new Label(parent);
   cameraLabel.text = "Camera:";
   cameraLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   cameraLabel.minWidth = 60;

   var cameraValue = new Label(parent);
   cameraValue.text = cameraMatch ? cameraMatch.name : (metadata.camera || "Unknown");

   var cameraConfidence = new Label(parent);
   if (cameraMatch) {
      cameraConfidence.text = "(" + (cameraMatch.confidence * 100).toFixed(0) + "%)";
      cameraConfidence.textColor = cameraMatch.confidence > 0.8 ? 0xFF008800 : 0xFFCC8800;
   }

   var cameraSizer = new HorizontalSizer;
   cameraSizer.spacing = 6;
   cameraSizer.add(cameraLabel);
   cameraSizer.add(cameraValue, 100);
   cameraSizer.add(cameraConfidence);

   var filterLabel = new Label(parent);
   filterLabel.text = "Filter:";
   filterLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   filterLabel.minWidth = 60;

   var filterValue = new Label(parent);
   filterValue.text = filterMatch ? filterMatch.name : (metadata.filter || "None");

   var filterConfidence = new Label(parent);
   if (filterMatch) {
      filterConfidence.text = "(" + (filterMatch.confidence * 100).toFixed(0) + "%)";
      filterConfidence.textColor = filterMatch.confidence > 0.8 ? 0xFF008800 : 0xFFCC8800;
   }

   var filterSizer = new HorizontalSizer;
   filterSizer.spacing = 6;
   filterSizer.add(filterLabel);
   filterSizer.add(filterValue, 100);
   filterSizer.add(filterConfidence);

   var statusLabel = new Label(parent);
   statusLabel.text = "Status:";
   statusLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;
   statusLabel.minWidth = 60;

   var statusValue = new Label(parent);
   var hasFullData = cameraMatch && cameraMatch.sensor && cameraMatch.confidence > 0.7;
   if (hasFullData) {
      statusValue.text = "Full spectral data available";
      statusValue.textColor = 0xFF008800;
   } else if (cameraMatch) {
      statusValue.text = "Partial data - using estimates";
      statusValue.textColor = 0xFFCC8800;
   } else {
      statusValue.text = "Unknown hardware - using defaults";
      statusValue.textColor = 0xFFCC0000;
   }

   var statusSizer = new HorizontalSizer;
   statusSizer.spacing = 6;
   statusSizer.add(statusLabel);
   statusSizer.add(statusValue, 100);

   var mainSizer = new VerticalSizer;
   mainSizer.spacing = 4;
   mainSizer.add(cameraSizer);
   mainSizer.add(filterSizer);
   mainSizer.add(statusSizer);

   return {
      sizer: mainSizer,
      cameraValue: cameraValue,
      filterValue: filterValue,
      statusValue: statusValue,
      updateCamera: function(match) {
         cameraValue.text = match ? match.name : "Unknown";
         if (match) {
            cameraConfidence.text = "(" + (match.confidence * 100).toFixed(0) + "%)";
            cameraConfidence.textColor = match.confidence > 0.8 ? 0xFF008800 : 0xFFCC8800;
         }
      },
      updateFilter: function(match) {
         filterValue.text = match ? match.name : "None";
         if (match) {
            filterConfidence.text = "(" + (match.confidence * 100).toFixed(0) + "%)";
            filterConfidence.textColor = match.confidence > 0.8 ? 0xFF008800 : 0xFFCC8800;
         }
      },
      updateStatus: function(text, isGood) {
         statusValue.text = text;
         statusValue.textColor = isGood ? 0xFF008800 : 0xFFCC8800;
      }
   };
}

/**
 * Show an info message box.
 *
 * @param {string} message - Message text
 * @param {string} title - Dialog title
 */
function showInfo(message, title) {
   var msg = new MessageBox(message, title || ScriptInfo.name, StdIcon_Information);
   msg.execute();
}

/**
 * Show a warning message box.
 *
 * @param {string} message - Message text
 * @param {string} title - Dialog title
 */
function showWarning(message, title) {
   var msg = new MessageBox(message, title || "Warning", StdIcon_Warning);
   msg.execute();
}

/**
 * Show an error message box.
 *
 * @param {string} message - Message text
 * @param {string} title - Dialog title
 */
function showError(message, title) {
   var msg = new MessageBox(message, title || "Error", StdIcon_Error);
   msg.execute();
}

/**
 * Show a confirmation dialog.
 *
 * @param {string} message - Message text
 * @param {string} title - Dialog title
 * @returns {boolean} True if user clicked Yes
 */
function confirm(message, title) {
   var msg = new MessageBox(message, title || "Confirm",
      StdIcon_Question, StdButton_Yes, StdButton_No);
   return msg.execute() === StdButton_Yes;
}
