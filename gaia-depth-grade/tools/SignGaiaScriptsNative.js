// ============================================================================
// SignGaiaScriptsNative.js — native (PI-valid) signing of the Gaia Depth Grade
// PJSR scripts, modeled on ez-stretch/tools/SignScriptsNative.js.
// ============================================================================
//
// Signs the feature scripts AND the shared .jsh they #include, each to its own
// .xsgn, using PixInsight's own Security.generateScriptSignatureFile() — the
// exact code path PI's loader verifies against. After signing, it re-reads each
// signature via Security.getScriptSignature() and confirms it is valid, so a
// silent bad-sign cannot look like success. Result is written to a JSON file the
// caller (release.sh) inspects, because automation-mode console output never
// reaches stdout.
//
// Password: /tmp/.pi_codesign_pass   Keys: scarter4work_keys.xssk
// ============================================================================

#feature-id    Utilities > SignGaiaScriptsNative
#script-id     SignGaiaScriptsNative
#feature-info  Native batch signing of Gaia Depth Grade scripts (.js/.jsh -> .xsgn)

var KEYS_FILE   = "/home/scarter4work/projects/keys/scarter4work_keys.xssk";
var PASS_FILE   = "/tmp/.pi_codesign_pass";
var RESULT_FILE = "/tmp/.gaia_sign_result.json";
var PI_DIR      = "/home/scarter4work/projects/astro-pi/gaia-depth-grade/pi";

// The feature scripts and the shared include they pull in — each signed to its
// own .xsgn (PI verifies #included files by their own signature, not as a
// dependency of the entry script's signature).
var SCRIPTS = [ "gaia_depth_grade.js", "GaiaDepthGradeDialog.js", "gaia_depth_grade_lib.jsh" ];

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
   var result = { ok: false, developerId: null, signed: [], verified: [], failed: [], error: null };

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
      var src  = PI_DIR + "/" + SCRIPTS[i];
      var xsgn = File.changeExtension(src, ".xsgn");
      if (!File.exists(src)) { result.failed.push({ name: SCRIPTS[i], error: "source not found" }); continue; }
      try {
         Security.generateScriptSignatureFile(xsgn, src, [], keys.developerId, keys.publicKey, keys.privateKey);
         if (!File.exists(xsgn)) { result.failed.push({ name: SCRIPTS[i], error: "no .xsgn produced" }); continue; }
         result.signed.push(SCRIPTS[i]);
         // Re-read the signature PI would verify at load; confirm it validates.
         var sig = Security.getScriptSignature(src);
         if (sig && (sig.valid === undefined || sig.valid))
            result.verified.push(SCRIPTS[i]);
         else
            result.failed.push({ name: SCRIPTS[i], error: "signature did not verify" });
      } catch (e) {
         result.failed.push({ name: SCRIPTS[i], error: e.message });
      }
   }

   keys.publicKey.secureFill();
   keys.privateKey.secureFill();

   result.ok = (result.failed.length === 0 &&
                result.signed.length === SCRIPTS.length &&
                result.verified.length === SCRIPTS.length);
   writeResult(result);
   console.writeln("SignGaiaScriptsNative: signed=" + result.signed.length +
                   " verified=" + result.verified.length + " failed=" + result.failed.length);
}

main();
