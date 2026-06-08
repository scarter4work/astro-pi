// test_module_load.js
// Test script to check if NukeX module is loaded in PixInsight

#include <pjsr/StdButton.jsh>

function main() {
   console.writeln("Testing NukeX module load...");
   console.writeln("");

   // List all loaded modules
   var modules = ProcessInstance.modules();
   console.writeln("Loaded modules: " + modules.length);

   var nukeXFound = false;
   for (var i = 0; i < modules.length; i++) {
      if (modules[i].indexOf("NukeX") >= 0) {
         console.writeln("  FOUND: " + modules[i]);
         nukeXFound = true;
      }
   }

   if (!nukeXFound) {
      console.warningln("NukeX module NOT found in loaded modules!");
   }

   console.writeln("");

   // Try to create NukeX process
   console.writeln("Testing NukeX process creation...");
   try {
      var P = new NukeX;
      console.writeln("  NukeX process created successfully!");
      console.writeln("  Algorithm: " + P.algorithm);
   } catch (e) {
      console.criticalln("  NukeX process FAILED: " + e.message);
   }

   // Try to create NukeXStack process
   console.writeln("");
   console.writeln("Testing NukeXStack process creation...");
   try {
      var P2 = new NukeXStack;
      console.writeln("  NukeXStack process created successfully!");
      console.writeln("  Selection strategy: " + P2.selectionStrategy);
   } catch (e) {
      console.criticalln("  NukeXStack process FAILED: " + e.message);
   }

   console.writeln("");
   console.writeln("Test complete.");
}

main();
