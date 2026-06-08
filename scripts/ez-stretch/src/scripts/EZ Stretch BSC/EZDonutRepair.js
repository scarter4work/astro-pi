// ============================================================================
// EZDonutRepair.js - Dust Donut Repair for Color and Mono Images
// ============================================================================
//
// Based on DonutRepair by Chick Dee (https://github.com/chickadeebird)
// Original work licensed under CC BY-NC 4.0
// Extended to support RGB color images by processing each channel separately.
//
// Original Copyright (c) 2024 Chick Dee
// Modifications Copyright (c) 2026 EZ Stretch BSC
//
// This work is licensed under CC BY-NC 4.0
// http://creativecommons.org/licenses/by-nc/4.0/
//
// ============================================================================

#feature-id    EZ Stretch BSC > EZ Donut Repair
#script-id     EZDonutRepair
#feature-info  Repairs circular dust donuts in both color and mono images. \
               Based on DonutRepair by Chick Dee, extended for RGB support.

#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/StdCursor.jsh>
#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/ImageOp.jsh>
#include <pjsr/SampleType.jsh>
#include <pjsr/UndoFlag.jsh>
#include <pjsr/TextAlign.jsh>
#include <pjsr/ButtonCodes.jsh>

#define TITLE "EZ Donut Repair"
#define VERSION "1.0.2"

var jsAutoGC = true;

// ============================================================================
// Parameters
// ============================================================================

let parameters = {
   targetWindow: null,
   previewZoomLevel: "Fit to Preview",
   shapes: [],
   shapeTypes: [],
   save: function() {
      Parameters.set("shapes", JSON.stringify(this.shapes));
      Parameters.set("shapeTypes", JSON.stringify(this.shapeTypes));
      Parameters.set("previewZoomLevel", this.previewZoomLevel);
      if (this.targetWindow) {
         Parameters.set("targetWindow", this.targetWindow.mainView.id);
      }
   },
   load: function() {
      if (Parameters.has("shapes")) {
         this.shapes = JSON.parse(Parameters.getString("shapes"));
      }
      if (Parameters.has("shapeTypes")) {
         this.shapeTypes = JSON.parse(Parameters.getString("shapeTypes"));
      }
      if (Parameters.has("previewZoomLevel")) {
         this.previewZoomLevel = Parameters.getString("previewZoomLevel");
      }
      if (Parameters.has("targetWindow")) {
         let windowId = Parameters.getString("targetWindow");
         let window = ImageWindow.windowById(windowId);
         if (window && !window.isNull) {
            this.targetWindow = window;
         }
      }
   }
};

// ============================================================================
// ScrollControl - Preview with shape drawing
// ============================================================================

function ScrollControl(parent) {
   this.__base__ = ScrollBox;
   this.__base__(parent);

   this.autoScroll = true;
   this.tracking = true;

   this.displayImage = null;
   this.dragging = false;
   this.dragOrigin = new Point(0);
   this.isDrawing = false;
   this.isMoving = false;
   this.isTransforming = false;
   this.currentShape = [];
   this.originalShape = [];
   this.shapes = [];
   this.shapeTypes = [];
   this.activeShapeIndex = 0;
   this.scrollPosition = new Point(0, 0);
   this.previousZoomLevel = 1;
   this.shapeType = "Ellipse";
   this.transformCenter = null;
   this.initialDistance = null;
   this.initialAngle = null;

   this.viewport.cursor = new Cursor(StdCursor_Cross);

   this.zoomFactor = 1;
   this.minZoomFactor = 0.1;
   this.maxZoomFactor = 10;

   this.getImage = function() {
      return this.displayImage;
   };

   this.doUpdateImage = function(image) {
      this.displayImage = image;
      this.initScrollBars();
      if (this.viewport) {
         this.viewport.update();
      }
   };

   this.initScrollBars = function(scrollPoint) {
      var image = this.getImage();
      if (image == null || image.width <= 0 || image.height <= 0) {
         this.setHorizontalScrollRange(0, 0);
         this.setVerticalScrollRange(0, 0);
         this.scrollPosition = new Point(0, 0);
      } else {
         let zoomFactor = this.zoomFactor;
         this.setHorizontalScrollRange(0, Math.max(0, (image.width * zoomFactor)));
         this.setVerticalScrollRange(0, Math.max(0, (image.height * zoomFactor)));
         if (scrollPoint) {
            this.scrollPosition = scrollPoint;
         } else {
            this.scrollPosition = new Point(
               Math.min(this.scrollPosition.x, (image.width * zoomFactor)),
               Math.min(this.scrollPosition.y, (image.height * zoomFactor))
            );
         }
      }
      if (this.viewport) {
         this.viewport.update();
      }
   };

   this.calculateTransformCenter = function(shape) {
      let sumX = 0;
      let sumY = 0;
      shape.forEach(function(point) {
         sumX += point[0];
         sumY += point[1];
      });
      return [sumX / shape.length, sumY / shape.length];
   };

   this.calculateDistance = function(x1, y1, x2, y2) {
      return Math.sqrt(Math.pow(x2 - x1, 2) + Math.pow(y2 - y1, 2));
   };

   this.calculateAngle = function(x1, y1, x2, y2) {
      return Math.atan2(y2 - y1, x2 - x1);
   };

   this.transformShape = function(shape, angle, scaleX, scaleY, centerX, centerY) {
      return shape.map(function(point) {
         let translatedX = point[0] - centerX;
         let translatedY = point[1] - centerY;
         let rotatedX = translatedX * Math.cos(angle) - translatedY * Math.sin(angle);
         let rotatedY = translatedX * Math.sin(angle) + translatedY * Math.cos(angle);
         let resizedX = rotatedX * scaleX;
         let resizedY = rotatedY * scaleY;
         return [resizedX + centerX, resizedY + centerY];
      });
   };

   var self = this;

   this.viewport.onMousePress = function(x, y, button, buttonState, modifiers) {
      let zoomFactor = self.zoomFactor;
      let adjustedX = (x / zoomFactor) + self.scrollPosition.x;
      let adjustedY = (y / zoomFactor) + self.scrollPosition.y;

      if (modifiers === 1) { // Shift key - draw
         self.startX = adjustedX;
         self.startY = adjustedY;
         self.isDrawing = true;
         self.dragging = false;
         self.currentShape = [[self.startX, self.startY], [self.startX, self.startY]];
      } else if (modifiers === 2 && self.shapes.length > 0 && self.shapes[self.activeShapeIndex]) { // Ctrl key - move
         self.startX = adjustedX;
         self.startY = adjustedY;
         self.isMoving = true;
         self.dragging = false;
         self.originalShape = [];
         for (let i = 0; i < self.shapes[self.activeShapeIndex].length; i++) {
            self.originalShape.push(self.shapes[self.activeShapeIndex][i].slice());
         }
      } else if (modifiers === 4 && self.shapes.length > 0 && self.shapes[self.activeShapeIndex]) { // Alt key - transform
         self.startX = adjustedX;
         self.startY = adjustedY;
         self.isTransforming = true;
         self.transformCenter = self.calculateTransformCenter(self.shapes[self.activeShapeIndex]);
         self.initialAngle = self.calculateAngle(self.transformCenter[0], self.transformCenter[1], self.startX, self.startY);
         self.initialDistance = self.calculateDistance(self.startX, self.startY, self.transformCenter[0], self.transformCenter[1]);
         self.originalShape = [];
         for (let i = 0; i < self.shapes[self.activeShapeIndex].length; i++) {
            self.originalShape.push(self.shapes[self.activeShapeIndex][i].slice());
         }
      } else {
         this.cursor = new Cursor(StdCursor_ClosedHand);
         self.dragOrigin.x = x;
         self.dragOrigin.y = y;
         self.dragging = true;
      }
   };

   this.viewport.onMouseMove = function(x, y, buttonState, modifiers) {
      let zoomFactor = self.zoomFactor;
      let adjustedX = (x / zoomFactor) + self.scrollPosition.x;
      let adjustedY = (y / zoomFactor) + self.scrollPosition.y;

      if (self.isDrawing) {
         let endX = adjustedX;
         let endY = adjustedY;
         let centerX = (self.startX + endX) / 2;
         let centerY = (self.startY + endY) / 2;
         let radiusX = Math.abs(endX - self.startX) / 2;
         let radiusY = Math.abs(endY - self.startY) / 2;
         self.currentShape = [];
         for (let angle = 0; angle < 2 * Math.PI; angle += 0.01) {
            self.currentShape.push([
               centerX + radiusX * Math.cos(angle),
               centerY + radiusY * Math.sin(angle)
            ]);
         }
         self.currentShape.push(self.currentShape[0]);
         if (self.viewport) {
            self.viewport.update();
         }
      } else if (self.isMoving) {
         let dx = adjustedX - self.startX;
         let dy = adjustedY - self.startY;
         self.shapes[self.activeShapeIndex] = self.originalShape.map(function(point) {
            return [point[0] + dx, point[1] + dy];
         });
         if (self.viewport) {
            self.viewport.update();
         }
      } else if (self.isTransforming) {
         let currentAngle = self.calculateAngle(self.transformCenter[0], self.transformCenter[1], adjustedX, adjustedY);
         let angleDifference = currentAngle - self.initialAngle;
         let currentDistance = self.calculateDistance(adjustedX, adjustedY, self.transformCenter[0], self.transformCenter[1]);
         let scale = currentDistance / self.initialDistance;
         self.shapes[self.activeShapeIndex] = self.transformShape(self.originalShape, angleDifference, scale, scale, self.transformCenter[0], self.transformCenter[1]);
         if (self.viewport) {
            self.viewport.update();
         }
      } else if (self.dragging) {
         let dx = (self.dragOrigin.x - x) / zoomFactor;
         let dy = (self.dragOrigin.y - y) / zoomFactor;
         self.scrollPosition = new Point(self.scrollPosition.x + dx, self.scrollPosition.y + dy);
         self.dragOrigin.x = x;
         self.dragOrigin.y = y;
         if (self.viewport) {
            self.viewport.update();
         }
      }
   };

   this.viewport.onMouseRelease = function(x, y, button, buttonState, modifiers) {
      if (self.isDrawing) {
         self.isDrawing = false;
         if (self.shapes.length > 0) {
            self.shapes.length = 0;
         }
         self.shapes.push(self.currentShape.filter(function(point) {
            return !isNaN(point[0]) && !isNaN(point[1]);
         }));
         self.shapeTypes.push(self.shapeType);
         self.currentShape = [];
         self.activeShapeIndex = self.shapes.length - 1;
         if (self.viewport) {
            self.viewport.update();
         }
      } else if (self.isMoving) {
         self.isMoving = false;
         if (self.viewport) {
            self.viewport.update();
         }
      } else if (self.isTransforming) {
         self.isTransforming = false;
         if (self.viewport) {
            self.viewport.update();
         }
      } else {
         this.cursor = new Cursor(StdCursor_Cross);
         self.dragging = false;
      }
   };

   this.viewport.onMouseWheel = function(x, y, delta, buttonState, modifiers) {
      if (!self.displayImage) {
         return;
      }

      let oldZoomFactor = self.zoomFactor;
      let maxHorizontalScroll = (self.displayImage.width * oldZoomFactor) - self.viewport.width;
      let maxVerticalScroll = (self.displayImage.height * oldZoomFactor) - self.viewport.height;
      let oldScrollPercentageX = self.scrollPosition.x / maxHorizontalScroll;
      let oldScrollPercentageY = self.scrollPosition.y / maxVerticalScroll;

      if (delta > 0) {
         self.zoomFactor = Math.min(self.zoomFactor * 1.25, self.maxZoomFactor);
      } else if (delta < 0) {
         self.zoomFactor = Math.max(self.zoomFactor * 0.8, self.minZoomFactor);
      }
      let newZoomFactor = self.zoomFactor;

      self.initScrollBars();

      maxHorizontalScroll = (self.displayImage.width * newZoomFactor) - self.viewport.width;
      maxVerticalScroll = (self.displayImage.height * newZoomFactor) - self.viewport.height;
      let newScrollPositionX = oldScrollPercentageX * maxHorizontalScroll;
      let newScrollPositionY = oldScrollPercentageY * maxVerticalScroll;

      newScrollPositionX = Math.max(0, Math.min(newScrollPositionX, maxHorizontalScroll));
      newScrollPositionY = Math.max(0, Math.min(newScrollPositionY, maxVerticalScroll));

      self.scrollPosition = new Point(newScrollPositionX, newScrollPositionY);
      self.viewport.update();
   };

   this.viewport.onPaint = function(x0, y0, x1, y1) {
      var g = new Graphics(this);
      var result = self.getImage();
      let zoomFactor = self.zoomFactor;

      if (result == null) {
         g.fillRect(x0, y0, x1, y1, new Brush(0xff000000));
      } else {
         g.scaleTransformation(zoomFactor);
         g.translateTransformation(-self.scrollPosition.x, -self.scrollPosition.y);
         g.drawBitmap(0, 0, result.render());

         self.shapes.forEach(function(shape, index) {
            g.pen = new Pen(index === self.activeShapeIndex ? 0xff00ff00 : 0xffff0000);
            for (let i = 0; i < shape.length - 1; i++) {
               if (Number.isFinite(shape[i][0]) && Number.isFinite(shape[i][1]) &&
                   Number.isFinite(shape[i + 1][0]) && Number.isFinite(shape[i + 1][1])) {
                  g.drawLine(shape[i][0], shape[i][1], shape[i + 1][0], shape[i + 1][1]);
               }
            }
         });

         if (self.currentShape.length > 0) {
            g.pen = new Pen(0xff00ff00);
            for (let i = 0; i < self.currentShape.length - 1; i++) {
               if (Number.isFinite(self.currentShape[i][0]) && Number.isFinite(self.currentShape[i][1]) &&
                   Number.isFinite(self.currentShape[i + 1][0]) && Number.isFinite(self.currentShape[i + 1][1])) {
                  g.drawLine(self.currentShape[i][0], self.currentShape[i][1],
                             self.currentShape[i + 1][0], self.currentShape[i + 1][1]);
               }
            }
         }
      }
      g.end();
   };

   this.initScrollBars();
}
ScrollControl.prototype = new ScrollBox;

// ============================================================================
// Main Dialog
// ============================================================================

function DonutRepairDialog() {
   this.__base__ = Dialog;
   this.__base__();

   var dialog = this;

   this.title_Lbl = new Label(this);
   this.title_Lbl.frameStyle = FrameStyle_Box;
   this.title_Lbl.margin = 6;
   this.title_Lbl.useRichText = true;
   this.title_Lbl.text = "<b>" + TITLE + " " + VERSION + "</b>";
   this.title_Lbl.textAlignment = TextAlign_Center;

   this.instructions_Lbl = new TextBox(this);
   this.instructions_Lbl.readOnly = true;
   this.instructions_Lbl.frameStyle = FrameStyle_Box;
   this.instructions_Lbl.text = "Instructions:\n\n" +
      "Shift + Click and drag to draw an ellipse around the donut.\n\n" +
      "CTRL + Click and Drag to MOVE the shape.\n" +
      "ALT + Click and Drag to ROTATE and RESIZE.\n\n" +
      "Supports both mono and RGB color images.\n\n" +
      "Based on DonutRepair by Chick Dee.";
   this.instructions_Lbl.setScaledMinWidth(320);

   let currentWindowName = ImageWindow.activeWindow ? ImageWindow.activeWindow.mainView.id : "";

   this.imageLabel = new Label(this);
   this.imageLabel.text = "Select Image:";
   this.imageLabel.textAlignment = TextAlign_Left | TextAlign_VertCenter;

   this.windowSelector_Cb = new ComboBox(this);
   this.windowSelector_Cb.toolTip = "Select the window to process.";
   for (var i = 0; i < ImageWindow.windows.length; i++) {
      this.windowSelector_Cb.addItem(ImageWindow.windows[i].mainView.id);
      if (ImageWindow.windows[i].mainView.id == currentWindowName) {
         this.windowSelector_Cb.currentItem = i;
         let window = ImageWindow.windowById(currentWindowName);
         if (window && !window.isNull) {
            parameters.targetWindow = window;
         }
      }
   }

   this.windowSelector_Cb.onItemSelected = function(index) {
      if (index >= 0) {
         let window = ImageWindow.windowById(dialog.windowSelector_Cb.itemText(index));
         if (window && !window.isNull) {
            parameters.targetWindow = window;
            let selectedImage = window.mainView.image;
            if (selectedImage) {
               var tmpImage = dialog.createAndDisplayTemporaryImage(selectedImage);
               dialog.previewControl.displayImage = tmpImage;
               dialog.previewControl.initScrollBars();
               dialog.previewControl.viewport.update();
               dialog.previousZoomLevel = dialog.downsamplingFactor;
            }
         }
      }
   };

   this.imageSelectionSizer = new HorizontalSizer;
   this.imageSelectionSizer.spacing = 4;
   this.imageSelectionSizer.add(this.imageLabel);
   this.imageSelectionSizer.add(this.windowSelector_Cb, 1);

   this.autoSTF_Cb = new CheckBox(this);
   this.autoSTF_Cb.text = "AutoSTF Preview";
   this.autoSTF_Cb.checked = true;
   this.autoSTF_Cb.onCheck = function() {
      if (parameters.targetWindow) {
         var selectedImage = parameters.targetWindow.mainView.image;
         if (selectedImage) {
            let tmpImage = dialog.createAndDisplayTemporaryImage(selectedImage);
            dialog.previewControl.displayImage = tmpImage;
            dialog.previewControl.viewport.update();
         }
      }
   };

   this.zoomLabel = new Label(this);
   this.zoomLabel.text = "Preview Zoom: ";
   this.zoomLabel.textAlignment = TextAlign_Right | TextAlign_VertCenter;

   this.zoomLevelComboBox = new ComboBox(this);
   this.zoomLevelComboBox.addItem("1:1");
   this.zoomLevelComboBox.addItem("1:2");
   this.zoomLevelComboBox.addItem("1:4");
   this.zoomLevelComboBox.addItem("1:8");
   this.zoomLevelComboBox.addItem("Fit to Preview");
   this.zoomLevelComboBox.currentItem = 1;
   this.previousZoomLevel = 1;

   this.zoomLevelComboBox.onItemSelected = function(index) {
      if (parameters.targetWindow) {
         var selectedImage = parameters.targetWindow.mainView.image;
         if (selectedImage) {
            let tmpImage = dialog.createAndDisplayTemporaryImage(selectedImage);
            dialog.previewControl.displayImage = tmpImage;
            dialog.previewControl.viewport.update();
         }
      }
   };

   this.resetButton = new ToolButton(this);
   this.resetButton.icon = this.scaledResource(":/icons/delete.png");
   this.resetButton.toolTip = "Clear all shapes";
   this.resetButton.onMousePress = function() {
      dialog.previewControl.shapes = [];
      dialog.previewControl.viewport.update();
   };

   this.zoomSizer = new HorizontalSizer;
   this.zoomSizer.spacing = 4;
   this.zoomSizer.add(this.autoSTF_Cb);
   this.zoomSizer.addStretch();
   this.zoomSizer.add(this.zoomLabel);
   this.zoomSizer.add(this.zoomLevelComboBox);
   this.zoomSizer.add(this.resetButton);

   this.authorship_Lbl = new Label(this);
   this.authorship_Lbl.frameStyle = FrameStyle_Box;
   this.authorship_Lbl.margin = 6;
   this.authorship_Lbl.useRichText = true;
   this.authorship_Lbl.text = "Based on DonutRepair by Chick Dee<br>" +
      "<a href=\"https://github.com/chickadeebird\">github.com/chickadeebird</a><br>" +
      "Extended for RGB by EZ Stretch BSC";
   this.authorship_Lbl.textAlignment = TextAlign_Center;

   this.newInstance_Btn = new ToolButton(this);
   this.newInstance_Btn.icon = this.scaledResource(":/process-interface/new-instance.png");
   this.newInstance_Btn.setScaledFixedSize(24, 24);
   this.newInstance_Btn.toolTip = "Create new instance with current parameters.";
   this.newInstance_Btn.onMousePress = function() {
      parameters.save();
      dialog.newInstance();
   };

   this.execute_Btn = new PushButton(this);
   this.execute_Btn.text = "Execute";
   this.execute_Btn.toolTip = "Repair the donut in the selected area.";
   this.execute_Btn.onClick = function() {
      if (parameters.targetWindow) {
         let selectedImage = parameters.targetWindow.mainView.image;
         if (selectedImage) {
            dialog.repairDonut(selectedImage);
         } else {
            console.criticalln("No image selected.");
         }
      } else {
         console.criticalln("No window selected.");
      }
   };

   this.close_Btn = new PushButton(this);
   this.close_Btn.text = "Close";
   this.close_Btn.onClick = function() {
      dialog.cancel();
   };

   this.buttonSizer = new HorizontalSizer;
   this.buttonSizer.spacing = 6;
   this.buttonSizer.add(this.newInstance_Btn);
   this.buttonSizer.addStretch();
   this.buttonSizer.add(this.execute_Btn);
   this.buttonSizer.add(this.close_Btn);

   this.previewControl = new ScrollControl(this);
   this.previewControl.setMinWidth(640);
   this.previewControl.setMinHeight(480);

   this.zoomInstructionLabel = new Label(this);
   this.zoomInstructionLabel.text = "Mouse wheel to zoom, drag to pan";
   this.zoomInstructionLabel.textAlignment = TextAlign_Center;

   this.previewSizer = new VerticalSizer;
   this.previewSizer.spacing = 4;
   this.previewSizer.add(this.zoomInstructionLabel);
   this.previewSizer.add(this.previewControl, 1);

   this.leftSizer = new VerticalSizer;
   this.leftSizer.spacing = 6;
   this.leftSizer.add(this.title_Lbl);
   this.leftSizer.addSpacing(6);
   this.leftSizer.add(this.instructions_Lbl);
   this.leftSizer.addSpacing(6);
   this.leftSizer.add(this.imageSelectionSizer);
   this.leftSizer.addSpacing(6);
   this.leftSizer.add(this.zoomSizer);
   this.leftSizer.addSpacing(6);
   this.leftSizer.add(this.authorship_Lbl);
   this.leftSizer.addStretch();
   this.leftSizer.add(this.buttonSizer);

   this.leftPanel = new Control(this);
   this.leftPanel.sizer = this.leftSizer;
   this.leftPanel.setFixedWidth(340);

   this.mainSizer = new HorizontalSizer;
   this.mainSizer.margin = 8;
   this.mainSizer.spacing = 8;
   this.mainSizer.add(this.leftPanel);
   this.mainSizer.add(this.previewSizer, 1);

   this.sizer = this.mainSizer;
   this.windowTitle = TITLE;
   this.adjustToContents();

   this.onShow = function() {
      if (dialog.windowSelector_Cb.currentItem >= 0) {
         let window = ImageWindow.windowById(dialog.windowSelector_Cb.itemText(dialog.windowSelector_Cb.currentItem));
         if (window && !window.isNull) {
            let selectedImage = window.mainView.image;
            if (selectedImage) {
               var tmpImage = dialog.createAndDisplayTemporaryImage(selectedImage);
               dialog.previewControl.displayImage = tmpImage;
               dialog.previewControl.initScrollBars();
               dialog.previewControl.viewport.update();
               dialog.previousZoomLevel = dialog.downsamplingFactor;
            }
         }
      }
   };

   // =========================================================================
   // Create temporary preview image with optional STF
   // =========================================================================
   this.createAndDisplayTemporaryImage = function(selectedImage) {
      let window = new ImageWindow(selectedImage.width, selectedImage.height,
         selectedImage.numberOfChannels,
         selectedImage.bitsPerSample,
         selectedImage.isReal,
         selectedImage.isColor
      );

      window.mainView.beginProcess();
      window.mainView.image.assign(selectedImage);
      window.mainView.endProcess();

      if (this.autoSTF_Cb.checked) {
         var P = new PixelMath;
         P.expression =
            "C = -2.8;\n" +
            "B = 0.20;\n" +
            "c = min(max(0,med($T)+C*1.4826*mdev($T)),1);\n" +
            "mtf(mtf(B,med($T)-c),max(0,($T-c)/~c))";
         P.useSingleExpression = true;
         P.symbols = "C,B,c";
         P.createNewImage = false;
         P.truncate = true;
         P.truncateLower = 0;
         P.truncateUpper = 1;
         P.executeOn(window.mainView);
      }

      var P = new IntegerResample;
      switch (this.zoomLevelComboBox.currentItem) {
         case 0: P.zoomFactor = -1; this.downsamplingFactor = 1; break;
         case 1: P.zoomFactor = -2; this.downsamplingFactor = 2; break;
         case 2: P.zoomFactor = -4; this.downsamplingFactor = 4; break;
         case 3: P.zoomFactor = -8; this.downsamplingFactor = 8; break;
         case 4:
            const previewWidth = this.previewControl.width;
            const widthScale = Math.floor(selectedImage.width / previewWidth);
            P.zoomFactor = -Math.max(widthScale, 1);
            this.downsamplingFactor = Math.max(widthScale, 1);
            break;
         default:
            P.zoomFactor = -2;
            this.downsamplingFactor = 2;
            break;
      }

      P.executeOn(window.mainView);

      let resizedImage = new Image(window.mainView.image);
      window.forceClose();

      return resizedImage;
   };

   // =========================================================================
   // Repair donut - works for both mono and color images
   // =========================================================================
   this.repairDonut = function(selectedImage) {
      console.noteln("EZ Donut Repair initiated");

      if (this.previewControl.shapes.length < 1) {
         (new MessageBox("No donut selected. Draw an ellipse around the donut first.",
            TITLE, StdIcon_Error, StdButton_Ok)).execute();
         return;
      }

      let firstShape = this.previewControl.shapes[0];
      let scaleRatio = this.downsamplingFactor;

      // Find shape bounds
      let minY = firstShape[0][1], maxY = firstShape[0][1];
      let minX = firstShape[0][0], maxX = firstShape[0][0];
      for (let i = 1; i < firstShape.length; i++) {
         if (firstShape[i][1] < minY) minY = firstShape[i][1];
         if (firstShape[i][1] > maxY) maxY = firstShape[i][1];
         if (firstShape[i][0] < minX) minX = firstShape[i][0];
         if (firstShape[i][0] > maxX) maxX = firstShape[i][0];
      }

      let x0 = scaleRatio * minX;
      let x1 = scaleRatio * maxX;
      let y0 = scaleRatio * minY;
      let y1 = scaleRatio * maxY;

      let xRadius = (x1 - x0) / 2;
      let yRadius = (y1 - y0) / 2;
      let xCenter = (x0 + x1) / 2;
      let yCenter = (y0 + y1) / 2;

      // Background ring is OUTSIDE the drawn ellipse (1.15-1.25x radius)
      let BG_RING_MIN = 1.15;
      let BG_RING_MAX = 1.25;
      // Donut shadow ring is the dark part of the donut (0.85-1.0x radius)
      let DONUT_RING_MIN = 0.85;
      let DONUT_RING_MAX = 1.0;
      // Bounds for sampling
      let BOUND_FACTOR = 1.4;
      // Blend region for smooth edges
      let BLEND_MIN = 0.9;
      let BLEND_MAX = 1.1;

      let lowerBoundX = xCenter - BOUND_FACTOR * xRadius;
      let upperBoundX = xCenter + BOUND_FACTOR * xRadius;
      let lowerBoundY = yCenter - BOUND_FACTOR * yRadius;
      let upperBoundY = yCenter + BOUND_FACTOR * yRadius;

      if (lowerBoundX < 0) lowerBoundX = 0;
      if (lowerBoundY < 0) lowerBoundY = 0;
      if (upperBoundX > selectedImage.width) upperBoundX = selectedImage.width;
      if (upperBoundY > selectedImage.height) upperBoundY = selectedImage.height;

      // Helper: check if point is in ellipse
      function inEllipse(px, py, cx, cy, rx, ry) {
         let dx = px - cx;
         let dy = py - cy;
         return (dx * dx) / (rx * rx) + (dy * dy) / (ry * ry) <= 1;
      }

      // Helper: get ellipse distance factor (0 at center, 1 at edge)
      function ellipseFactor(px, py, cx, cy, rx, ry) {
         let dx = px - cx;
         let dy = py - cy;
         return Math.sqrt((dx * dx) / (rx * rx) + (dy * dy) / (ry * ry));
      }

      // Helper: median of array
      function medianOfList(arr) {
         if (arr.length === 0) return 0;
         arr.sort(function(a, b) { return a - b; });
         let mid = Math.floor(arr.length / 2);
         return arr.length % 2 !== 0 ? arr[mid] : (arr[mid - 1] + arr[mid]) / 2;
      }

      // Process each channel
      let numChannels = selectedImage.numberOfChannels;
      console.noteln("Processing " + numChannels + " channel(s)");

      parameters.targetWindow.mainView.beginProcess(UndoFlag_NoSwapFile);

      for (let ch = 0; ch < numChannels; ch++) {
         console.noteln("Processing channel " + ch);

         // Get BACKGROUND median (outside the donut)
         let bgList = [];
         for (let x = Math.floor(lowerBoundX); x < upperBoundX; x++) {
            for (let y = Math.floor(lowerBoundY); y < upperBoundY; y++) {
               let inOuter = inEllipse(x, y, xCenter, yCenter, xRadius * BG_RING_MAX, yRadius * BG_RING_MAX);
               let inInner = inEllipse(x, y, xCenter, yCenter, xRadius * BG_RING_MIN, yRadius * BG_RING_MIN);
               if (inOuter && !inInner) {
                  bgList.push(selectedImage.sample(x, y, ch));
               }
            }
         }
         let bgMedian = medianOfList(bgList);

         // Get DONUT SHADOW median (the dark ring of the donut itself)
         let donutList = [];
         for (let x = Math.floor(lowerBoundX); x < upperBoundX; x++) {
            for (let y = Math.floor(lowerBoundY); y < upperBoundY; y++) {
               let inOuter = inEllipse(x, y, xCenter, yCenter, xRadius * DONUT_RING_MAX, yRadius * DONUT_RING_MAX);
               let inInner = inEllipse(x, y, xCenter, yCenter, xRadius * DONUT_RING_MIN, yRadius * DONUT_RING_MIN);
               if (inOuter && !inInner) {
                  donutList.push(selectedImage.sample(x, y, ch));
               }
            }
         }
         let donutMedian = medianOfList(donutList);

         if (donutMedian === 0) {
            console.warningln("Channel " + ch + ": donut median is 0, skipping");
            continue;
         }

         // Correction factor to bring donut shadow UP to background level
         let correctionFactor = bgMedian / donutMedian;
         console.noteln("Channel " + ch + ": bg=" + bgMedian.toFixed(6) +
                        ", donut=" + donutMedian.toFixed(6) +
                        ", correction=" + correctionFactor.toFixed(4));

         // Apply correction with radial falloff (more at edge, less at center)
         for (let x = Math.floor(lowerBoundX); x < upperBoundX; x++) {
            for (let y = Math.floor(lowerBoundY); y < upperBoundY; y++) {
               if (inEllipse(x, y, xCenter, yCenter, xRadius, yRadius)) {
                  let oldVal = selectedImage.sample(x, y, ch);
                  // Radial factor: 0 at center, 1 at edge
                  let radial = ellipseFactor(x, y, xCenter, yCenter, xRadius, yRadius);
                  // Apply stronger correction at edge (where the dark ring is)
                  let localCorrection = 1.0 + (correctionFactor - 1.0) * radial;
                  let newVal = Math.min(1.0, Math.max(0, oldVal * localCorrection));
                  parameters.targetWindow.mainView.image.setSample(newVal, x, y, ch);
               }
            }
         }

         // Blend edges smoothly with background
         for (let x = Math.floor(lowerBoundX); x < upperBoundX; x++) {
            for (let y = Math.floor(lowerBoundY); y < upperBoundY; y++) {
               let factor = ellipseFactor(x, y, xCenter, yCenter, xRadius, yRadius);
               if (factor >= BLEND_MIN && factor <= BLEND_MAX) {
                  let correctedVal = parameters.targetWindow.mainView.image.sample(x, y, ch);
                  let originalVal = selectedImage.sample(x, y, ch);
                  // Blend: 0 at BLEND_MIN (fully corrected), 1 at BLEND_MAX (fully original)
                  let blendRatio = (factor - BLEND_MIN) / (BLEND_MAX - BLEND_MIN);
                  let blendedVal = correctedVal * (1 - blendRatio) + originalVal * blendRatio;
                  parameters.targetWindow.mainView.image.setSample(blendedVal, x, y, ch);
               }
            }
         }
      }

      parameters.targetWindow.mainView.endProcess();
      console.noteln("Donut repair complete");
   };
}
DonutRepairDialog.prototype = new Dialog;

// ============================================================================
// Main
// ============================================================================

function main() {
   console.show();
   console.noteln(TITLE + " " + VERSION);
   console.noteln("Based on DonutRepair by Chick Dee");
   console.noteln("Extended for RGB support by EZ Stretch BSC");
   console.writeln("");

   if (ImageWindow.windows.length === 0) {
      (new MessageBox("No images are open. Please open an image first.",
         TITLE, StdIcon_Error, StdButton_Ok)).execute();
      return;
   }

   let dialog = new DonutRepairDialog();
   dialog.execute();
}

main();
