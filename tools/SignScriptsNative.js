// ============================================================================
// SignScriptsNative.js - Native (PI-valid) signing of EZ Stretch BSC scripts
// ============================================================================
//
// Signs ONLY the .js scripts (not the manifest) using PixInsight's own
// Security.generateScriptSignatureFile() API, so the resulting .xsgn files are
// verified by the exact code path PI uses at load time. This replaces the
// reverse-engineered Ed25519 signer (tools/pi_codesign.py), whose signatures
// were internally self-consistent but used a key/format PI does not trust.
//
// Invoked headlessly by tools/sign.sh. Because automation-mode console output
// does not reach stdout, this script writes a machine-readable RESULT_FILE that
// the caller inspects - a silent no-op therefore cannot look like success.
//
// Password: read from /tmp/.pi_codesign_pass
// Keys:     /home/scarter4work/projects/keys/scarter4work_keys.xssk
// ============================================================================

#feature-id    Utilities > SignScriptsNative
#script-id     SignScriptsNative
#feature-info  Native batch signing of EZ Stretch BSC scripts (.js -> .xsgn)

var KEYS_FILE   = "/home/scarter4work/projects/keys/scarter4work_keys.xssk";
var PASS_FILE   = "/tmp/.pi_codesign_pass";
var RESULT_FILE = "/tmp/.ez_sign_result.json";
var PROJECT_DIR = "/home/scarter4work/projects/EZ-suite-bsc/EZ-Stretch-BSC";
var SCRIPTS_DIR = PROJECT_DIR + "/src/scripts/EZ Stretch BSC";

var SCRIPTS = [ "EZStretch", "EZDonutRepair", "EZHazeKill" ];

function writeResult(obj) {
   try {
      var f = new File();
      f.createForWriting(RESULT_FILE);
      f.write(ByteArray.stringToUTF8(JSON.stringify(obj)));
      f.close();
   } catch (e) {
      console.criticalln("Could not write result file: " + e.message);
   }
}

function readPassword() {
   if (typeof jsArguments !== 'undefined' && jsArguments.length > 0 && jsArguments[0].length > 0)
      return jsArguments[0];
   if (File.exists(PASS_FILE))
      return File.readTextFile(PASS_FILE).trim();
   return null;
}

function main() {
   var result = { ok: false, developerId: null, signed: [], failed: [], error: null };

   // Start from a clean slate so a crash before completion is detectable.
   if (File.exists(RESULT_FILE))
      try { File.remove(RESULT_FILE); } catch (e) {}

   var password = readPassword();
   if (!password) { result.error = "no password at " + PASS_FILE; writeResult(result); return; }
   if (!File.exists(KEYS_FILE)) { result.error = "keys file missing: " + KEYS_FILE; writeResult(result); return; }

   var keys;
   try {
      keys = Security.loadSigningKeysFile(KEYS_FILE, password);
   } catch (e) {
      result.error = "loadSigningKeysFile threw: " + e.message; writeResult(result); return;
   }
   if (!keys || !keys.valid) { result.error = "invalid keys or wrong password"; writeResult(result); return; }
   result.developerId = keys.developerId;

   for (var i = 0; i < SCRIPTS.length; ++i) {
      var js   = SCRIPTS_DIR + "/" + SCRIPTS[i] + ".js";
      var xsgn = File.changeExtension(js, ".xsgn");
      if (!File.exists(js)) { result.failed.push({ name: SCRIPTS[i], error: "js not found" }); continue; }
      try {
         Security.generateScriptSignatureFile(xsgn, js, [], keys.developerId, keys.publicKey, keys.privateKey);
         if (File.exists(xsgn)) result.signed.push(SCRIPTS[i]);
         else                   result.failed.push({ name: SCRIPTS[i], error: "no .xsgn produced" });
      } catch (e) {
         result.failed.push({ name: SCRIPTS[i], error: e.message });
      }
   }

   keys.publicKey.secureFill();
   keys.privateKey.secureFill();

   result.ok = (result.failed.length === 0 && result.signed.length === SCRIPTS.length);
   writeResult(result);

   console.writeln("SignScriptsNative: signed=" + result.signed.length + " failed=" + result.failed.length);
}

main();
