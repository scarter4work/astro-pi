/*
 * Test script for NukeX pre-stretch functionality
 */

#include <pjsr/DataType.jsh>

function main()
{
   console.writeln("=== NukeX Pre-stretch Test ===");
   console.flush();

   // Find a test FITS file
   var testFiles = [
      "/home/scarter4work/.local/share/Trash/files/training/data/nasa_galaxy_segmentation/imagedata_1-100523.fits"
   ];

   var testFile = null;
   for (var i = 0; i < testFiles.length; i++) {
      if (File.exists(testFiles[i])) {
         testFile = testFiles[i];
         break;
      }
   }

   if (testFile == null) {
      console.criticalln("No test file found!");
      return;
   }

   console.writeln("Test file: " + testFile);
   console.flush();

   // Open the image
   console.writeln("Opening image...");
   console.flush();

   var windows = ImageWindow.open(testFile);
   if (windows.length == 0) {
      console.criticalln("Failed to open image!");
      return;
   }

   var window = windows[0];
   console.writeln("Image opened: " + window.mainView.id);
   console.writeln("Dimensions: " + window.mainView.image.width + " x " + window.mainView.image.height);
   console.flush();

   // Try to create and execute NukeXStack with pre-stretch
   console.writeln("Creating NukeXStack process...");
   console.flush();

   try {
      var P = new NukeXStack;
      P.inputFrames = [[true, testFile]];
      P.preStretchWithNukeX = true;
      P.preStretchStrength = 0.5;
      P.selectionStrategy = 0; // Distribution
      P.enableMLSegmentation = false;

      console.writeln("Executing NukeXStack...");
      console.flush();

      P.executeGlobal();

      console.writeln("SUCCESS: NukeXStack completed!");
   }
   catch (e) {
      console.criticalln("ERROR: " + e.message);
   }

   // Close the window
   window.close();

   console.writeln("=== Test Complete ===");
   console.flush();
}

main();
