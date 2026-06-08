// ============================================================================
// CLICodeSign.js - Headless code signing for EZ Stretch BSC
// ============================================================================
//
// Uses the same Security API as PixInsight's official CodeSign script.
// Set CODESIGN_PASS environment variable or CODESIGN_PASS_FILE before running.
//
// ============================================================================

#feature-id    Utilities > CLICodeSign
#script-id     CLICodeSign
#feature-info  Command-line code signing utility for batch signing scripts

// Project-specific configuration
var PROJECT_DIR = "/home/scarter4work/projects/EZ-suite-bsc/EZ-Stretch-BSC";
var DEFAULT_KEYS = "/home/scarter4work/projects/keys/scarter4work_keys.xssk";

// Files to sign - add new scripts here
var PROJECT_SCRIPTS = [
   PROJECT_DIR + "/src/scripts/EZ Stretch BSC/EZStretch.js",
   PROJECT_DIR + "/src/scripts/EZ Stretch BSC/EZDonutRepair.js",
   PROJECT_DIR + "/src/scripts/EZ Stretch BSC/EZHazeKill.js"
];

var XRI_FILES = [
   PROJECT_DIR + "/repository/updates.xri"
];

// Default password file location
var PASS_FILE = "/tmp/.pi_codesign_pass";

function getPassword() {
   // Method 1: Check jsArguments (passed via command line)
   if (typeof jsArguments !== 'undefined' && jsArguments.length > 0) {
      return jsArguments[0];
   }

   // Method 2: Read from temp file
   if (File.exists(PASS_FILE)) {
      try {
         var pass = File.readTextFile(PASS_FILE);
         return pass.trim();
      } catch (e) {
         console.warningln("Error reading password file: " + e.message);
      }
   }

   // Method 3: Try environment (may not work in all modes)
   try {
      var env = new ProcessEnvironment();
      if (env.has("CODESIGN_PASS")) {
         return env.get("CODESIGN_PASS");
      }
   } catch (e) {
      // ProcessEnvironment not available in execute mode
   }

   return null;
}

function signScriptFile(keys, filePath) {
   try {
      var signaturePath = File.changeExtension(filePath, ".xsgn");
      Security.generateScriptSignatureFile(
         signaturePath,
         filePath,
         [],  // entitlements
         keys.developerId,
         keys.publicKey,
         keys.privateKey
      );
      console.noteln("  Signed: " + File.extractName(filePath));
      return true;
   } catch (e) {
      console.criticalln("  FAILED: " + File.extractName(filePath));
      console.criticalln("  Error: " + e.message);
      return false;
   }
}

function signXRIFile(keys, filePath) {
   try {
      Security.generateXMLSignature(
         filePath,
         keys.developerId,
         keys.publicKey,
         keys.privateKey
      );
      console.noteln("  Signed: " + File.extractName(filePath));
      return true;
   } catch (e) {
      console.criticalln("  FAILED: " + File.extractName(filePath));
      console.criticalln("  Error: " + e.message);
      return false;
   }
}

function main() {
   console.writeln("");
   console.writeln("===========================================");
   console.writeln("CLICodeSign - EZ Stretch BSC Code Signing");
   console.writeln("===========================================");
   console.writeln("");

   var keysFile = DEFAULT_KEYS;
   var password = getPassword();

   if (!password) {
      console.criticalln("Error: No password provided");
      console.criticalln("Set CODESIGN_PASS or CODESIGN_PASS_FILE environment variable");
      return;
   }

   if (!File.exists(keysFile)) {
      console.criticalln("Error: Keys file not found: " + keysFile);
      return;
   }

   console.writeln("Keys file: " + keysFile);
   console.writeln("");

   // Load signing keys (this is how the official CodeSign does it)
   var keys;
   try {
      keys = Security.loadSigningKeysFile(keysFile, password);
      if (!keys.valid) {
         console.criticalln("Error: Invalid signing keys");
         return;
      }
      console.noteln("Loaded keys for developer: " + keys.developerId);
   } catch (e) {
      console.criticalln("Error loading keys: " + e.message);
      return;
   }

   var succeeded = 0;
   var failed = 0;

   // Sign JS files
   console.writeln("");
   console.writeln("Signing JavaScript files:");
   for (var i = 0; i < PROJECT_SCRIPTS.length; i++) {
      var filePath = PROJECT_SCRIPTS[i];
      if (!File.exists(filePath)) {
         console.warningln("  File not found: " + filePath);
         failed++;
         continue;
      }
      if (signScriptFile(keys, filePath)) {
         succeeded++;
      } else {
         failed++;
      }
   }

   // Sign XRI files
   console.writeln("");
   console.writeln("Signing XML files:");
   for (var i = 0; i < XRI_FILES.length; i++) {
      var filePath = XRI_FILES[i];
      if (!File.exists(filePath)) {
         console.warningln("  File not found: " + filePath);
         failed++;
         continue;
      }
      if (signXRIFile(keys, filePath)) {
         succeeded++;
      } else {
         failed++;
      }
   }

   // Securely clear keys
   keys.publicKey.secureFill();
   keys.privateKey.secureFill();

   console.writeln("");
   console.writeln("===========================================");
   console.writeln("Results: " + succeeded + " succeeded, " + failed + " failed");
   console.writeln("===========================================");
}

main();
