#include "../RCAstroLib.jsh"
// Regression test: the wrappers require rc-astro CLI >= 2.0. CLI 2.x renamed
// flags the wrappers emit (BlurXTerminator --ansr/--nsr -> --ansp/--nsd, radius
// -> diameter) and dropped models (NoiseXTerminator ML 3 -> 3.1), so an older
// CLI must be refused up front with a loud error naming the version found and
// the update steps -- never run with flags it will reject.
//
// Stand-in binaries answer the version probe (`--no-banner --help`, which both
// 0.9.x and 2.x print as "Version X.Y.Z (build ...)") and, for any other call,
// emit a clean completion event -- so a wrapper WITHOUT the gate "succeeds"
// against the 0.9.10 stand-in and this test goes RED.
//
// Result goes to a FILE -- console.* does not reach stdout under --automation-mode.
var RESULT_LOG = "/tmp/rc_t_version_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function makeStub(dir, name, helpLines) {
   let path = dir + "/" + name;
   let f = new File;
   f.createForWriting(path);
   f.outTextLn("#!/bin/sh");
   f.outTextLn("case \" $* \" in");
   f.outTextLn("  *\" --help \"*)");
   for (let i = 0; i < helpLines.length; ++i) f.outTextLn("    " + helpLines[i]);
   f.outTextLn("    exit 0 ;;");
   f.outTextLn("esac");
   f.outTextLn("printf '{\"event\":\"status\",\"phase\":\"complete\",\"message\":\"Done\"}\\n'");
   f.close();
   let chmod = new ExternalProcess;
   chmod.start("/bin/chmod 700 " + path);
   for (; chmod.isStarting;) processEvents();
   for (; chmod.isRunning;)  processEvents();
   assert(chmod.exitCode == 0, "chmod of " + name + " failed");
   return path;
}

// Run runCli against `bin`; returns {threw, message, result}.
function tryRun(bin) {
   RCAstro.binaryPath = bin;
   try {
      let r = RCAstro.runCli("nxt", [], null);
      return { threw: false, message: "", result: r };
   } catch (e) {
      return { threw: true, message: e.message, result: null };
   }
}

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }
   let savedBin = RCAstro.binaryPath;

   try {
      let dir = RCAstro.tempDir();

      // --- 0.9.10: refused, message names the version and the update steps ---
      let old = makeStub(dir, "rc-astro-0.9.10", [
         "echo 'Astronomical image processing tools'",
         "echo 'Version 0.9.10 (build 190, gc82af2b5, 2026-07-09)'" ]);
      let t1 = tryRun(old);
      W("0.9.10 stand-in: threw=" + t1.threw + " message=" + t1.message);
      assert(t1.threw, "runCli ran a 0.9.10 CLI instead of refusing it");
      assert(t1.message.indexOf("0.9.10") >= 0, "error does not name the version found: " + t1.message);
      assert(t1.message.indexOf("2.0") >= 0, "error does not name the required version: " + t1.message);
      assert(t1.message.indexOf("rc-astro update --install") >= 0, "error lacks the update command: " + t1.message);
      assert(t1.message.indexOf("install.sh") >= 0, "error lacks the install.sh step: " + t1.message);

      // --- 1.9.9: boundary below the minimum, refused ---
      let t2 = tryRun(makeStub(dir, "rc-astro-1.9.9", [ "echo 'Version 1.9.9 (build 1, g0, 2026-01-01)'" ]));
      assert(t2.threw && t2.message.indexOf("1.9.9") >= 0, "1.9.9 was not refused: " + t2.message);

      // --- no version at all: refused, never assumed compatible ---
      let t3 = tryRun(makeStub(dir, "rc-astro-noversion", [ "echo 'usage: something'" ]));
      W("no-version stand-in: threw=" + t3.threw + " message=" + t3.message);
      assert(t3.threw, "runCli ran a CLI that reports no version instead of refusing it");
      assert(t3.message.indexOf("rc-astro update --install") >= 0, "no-version error lacks the update command: " + t3.message);

      // --- 2.0.0 boundary and 2.6.9: accepted. The 2.6.9 stand-in only reports
      // its version when LD_LIBRARY_PATH is unset (mirrors the real binary,
      // which dies before main() on PixInsight's libonnxruntime), proving the
      // probe runs with the same scrubbed environment as the real run.
      let t4 = tryRun(makeStub(dir, "rc-astro-2.0.0", [ "echo 'Version 2.0.0 (build 1, g0, 2026-01-01)'" ]));
      assert(!t4.threw && t4.result.ok, "2.0.0 was refused: " + (t4.message || (t4.result && t4.result.errorMsg)));
      let t5 = tryRun(makeStub(dir, "rc-astro-2.6.9", [
         "if [ -n \"${LD_LIBRARY_PATH+x}\" ]; then echo \"libonnxruntime.so.1: version VERS_1.23.2 not found\"; exit 127; fi",
         "echo 'Version 2.6.9 (build 727, ga2033f5b, 2026-09-08)'" ]));
      assert(!t5.threw && t5.result.ok, "2.6.9 was refused: " + (t5.message || (t5.result && t5.result.errorMsg)));
      W("PASS: 2.0.0 and 2.6.9 stand-ins accepted");

      // --- a refused binary is not cached as good: re-running still refuses ---
      let t6 = tryRun(old);
      assert(t6.threw, "second run against 0.9.10 was not refused (bad cache?)");

      // --- the real installed CLI passes the gate ---
      RCAstro.binaryPath = null;
      let bin = RCAstro.findBinary();
      assert(bin, "real rc-astro not found");
      let v = RCAstro.requireCliVersion(bin);
      W("real CLI " + bin + " reports " + v);
      assert(/^[0-9]+\.[0-9]+\.[0-9]+$/.test(v) && Number(v.split(".")[0]) >= 2, "real CLI version not >= 2: " + v);

      RCAstro.binaryPath = savedBin;
      RCAstro.cleanup([]);
      W("PASS t_lib_version");
   } catch (e) {
      RCAstro.binaryPath = savedBin;
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
