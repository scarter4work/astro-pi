// DumpKeys.js - Extract signing keys for standalone signing tool
// Run in PixInsight to extract keys to JSON format
//
// Usage:
//   1. Write password to /tmp/.pi_codesign_pass
//   2. Run this script in PixInsight
//   3. Keys are written to ~/.pi_signing_keys.json (chmod 600)
//
// This version includes diagnostic test signing to help debug
// the standalone Python signing tool.

#feature-id    Utilities > DumpKeys
#script-id     DumpKeys

var KEYS_FILE = "/home/scarter4work/projects/keys/scarter4work_keys.xssk";
var PASS_FILE = "/tmp/.pi_codesign_pass";
var OUTPUT_FILE = "/tmp/.pi_signing_keys.json";
var DIAG_FILE = "/tmp/.pi_signing_diag.json";

function getPassword() {
   if (File.exists(PASS_FILE)) {
      try {
         var pass = File.readTextFile(PASS_FILE);
         return pass.trim();
      } catch (e) {
         console.warningln("Error reading password file: " + e.message);
      }
   }
   return null;
}

function byteArrayToHex(ba) {
   var hex = "";
   for (var i = 0; i < ba.length; i++) {
      var b = ba.at(i);
      hex += ("0" + (b & 0xff).toString(16)).slice(-2);
   }
   return hex;
}

function byteArrayToBase64(ba) {
   // Convert ByteArray to base64 using PixInsight's built-in method
   return ba.toBase64();
}

function main() {
   console.writeln("");
   console.writeln("===========================================");
   console.writeln("DumpKeys - Key Extraction & Diagnostics");
   console.writeln("===========================================");
   console.writeln("");

   var password = getPassword();
   if (!password) {
      console.criticalln("Error: No password. Write it to " + PASS_FILE);
      return;
   }

   if (!File.exists(KEYS_FILE)) {
      console.criticalln("Error: Keys file not found: " + KEYS_FILE);
      return;
   }

   console.writeln("Loading keys from: " + KEYS_FILE);

   try {
      var keys = Security.loadSigningKeysFile(KEYS_FILE, password);

      if (!keys.valid) {
         console.criticalln("Error: Invalid keys or wrong password");
         return;
      }

      console.noteln("Developer ID: " + keys.developerId);
      console.noteln("Public key length: " + keys.publicKey.length + " bytes");
      console.noteln("Private key length: " + keys.privateKey.length + " bytes");

      // Get hex representation
      var pubHex = byteArrayToHex(keys.publicKey);
      var privHex = byteArrayToHex(keys.privateKey);

      // Also get base64 representation (might be more reliable)
      var pubB64 = byteArrayToBase64(keys.publicKey);
      var privB64 = byteArrayToBase64(keys.privateKey);

      console.writeln("");
      console.writeln("Public key (hex):  " + pubHex.substring(0, 32) + "...");
      console.writeln("Public key (b64):  " + pubB64.substring(0, 32) + "...");
      console.writeln("Private key (hex): " + privHex.substring(0, 32) + "...");
      console.writeln("Private key (b64): " + privB64.substring(0, 32) + "...");

      // Ed25519 keys should be 32 bytes (public) and 32 or 64 bytes (private)
      if (keys.publicKey.length != 32) {
         console.warningln("Warning: Public key is " + keys.publicKey.length + " bytes, expected 32");
      }
      if (keys.privateKey.length != 32 && keys.privateKey.length != 64) {
         console.warningln("Warning: Private key is " + keys.privateKey.length + " bytes, expected 32 or 64");
      }

      // =========================================================
      // DIAGNOSTIC: Create a test signature using Security API
      // =========================================================
      console.writeln("");
      console.writeln("Creating diagnostic test signature...");

      // Create a simple test script file with required #script-id
      var testContent = "#script-id TestScript\n// Test content for signature verification.\nfunction main() {}\nmain();\n";
      var testScriptFile = "/tmp/.pi_test_sign.js";
      var testOut = new File();
      testOut.createForWriting(testScriptFile);
      testOut.write(ByteArray.stringToUTF8(testContent));
      testOut.close();

      // Sign the test file using PixInsight's native API
      var testSigFile = testScriptFile.replace(".js", ".xsgn");
      try {
         Security.generateScriptSignatureFile(
            testSigFile,
            testScriptFile,
            [],  // no entitlements
            keys.developerId,
            keys.publicKey,
            keys.privateKey
         );
         console.noteln("Test signature created: " + testSigFile);

         // Read back the signature file to get timestamp and signature
         var sigContent = File.readTextFile(testSigFile);
         console.writeln("Signature file content:");
         console.writeln(sigContent);

         // Also output the test content for verification
         console.writeln("");
         console.writeln("Test script content (exact bytes):");
         console.writeln(testContent);

      } catch (e) {
         console.warningln("Test signature failed: " + e.message);
      }

      // =========================================================
      // Write keys as JSON for Python tool
      // =========================================================
      var json = '{\n';
      json += '  "developerId": "' + keys.developerId + '",\n';
      json += '  "publicKey": "' + pubHex + '",\n';
      json += '  "privateKey": "' + privHex + '",\n';
      json += '  "publicKeyB64": "' + pubB64 + '",\n';
      json += '  "privateKeyB64": "' + privB64 + '"\n';
      json += '}\n';

      var out = new File();
      out.createForWriting(OUTPUT_FILE);
      out.write(ByteArray.stringToUTF8(json));
      out.close();

      // =========================================================
      // Write diagnostic info
      // =========================================================
      var diag = '{\n';
      diag += '  "developerId": "' + keys.developerId + '",\n';
      diag += '  "publicKeyLen": ' + keys.publicKey.length + ',\n';
      diag += '  "privateKeyLen": ' + keys.privateKey.length + ',\n';
      diag += '  "publicKeyHex": "' + pubHex + '",\n';
      diag += '  "privateKeyHex": "' + privHex + '",\n';
      diag += '  "publicKeyB64": "' + pubB64 + '",\n';
      diag += '  "privateKeyB64": "' + privB64 + '",\n';
      diag += '  "testFile": "' + testScriptFile + '",\n';
      diag += '  "testContent": "' + testContent.replace(/\n/g, '\\n') + '"\n';
      diag += '}\n';

      var diagOut = new File();
      diagOut.createForWriting(DIAG_FILE);
      diagOut.write(ByteArray.stringToUTF8(diag));
      diagOut.close();

      console.noteln("");
      console.noteln("Keys written to: " + OUTPUT_FILE);
      console.noteln("Diagnostics written to: " + DIAG_FILE);
      console.warningln("SECURITY: Move keys file to secure location!");
      console.writeln("");
      console.writeln("For standalone signing, run:");
      console.writeln("  mv /tmp/.pi_signing_keys.json ~/.pi_signing_keys.json");
      console.writeln("  chmod 600 ~/.pi_signing_keys.json");

      // Securely clear keys
      keys.publicKey.secureFill();
      keys.privateKey.secureFill();

   } catch (e) {
      console.criticalln("Error: " + e.message);
   }
}

main();
