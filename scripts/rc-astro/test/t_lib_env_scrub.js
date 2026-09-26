#include "../RCAstroLib.jsh"
// Regression test: the rc-astro child must NOT inherit PixInsight's own
// LD_LIBRARY_PATH. PixInsight.sh exports LD_LIBRARY_PATH=/opt/PixInsight/bin/lib:...,
// and since PI 1.9.x (MachineLearning module) that directory holds PI's own
// libonnxruntime.so.1 (1.25.1). rc-astro links libonnxruntime.so.1 through a
// RUNPATH of $ORIGIN, which LD_LIBRARY_PATH overrides, so an inherited path made
// every run die at load time with:
//   libonnxruntime.so.1: version `VERS_1.23.2' not found (required by rc-astro)
// run-headless.sh exports the same PI library path, so this test reproduces the
// real GUI launch environment.
//
// Result goes to a FILE -- console.* does not reach stdout under --automation-mode.
var RESULT_LOG = "/tmp/rc_t_env_scrub_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }
   let savedBin = RCAstro.binaryPath;

   try {
      // --- Case 1 (deterministic): a stand-in binary reports the environment ---
      // it was actually given. Proves the scrub itself, independent of which
      // onnxruntime versions happen to be installed.
      let dir = RCAstro.tempDir();
      let fake = dir + "/fake-rc-astro";
      let f = new File;
      f.createForWriting(fake);
      f.outTextLn("#!/bin/sh");
      f.outTextLn("printf '{\"event\":\"info\",\"topic\":\"env\",\"ldpath\":\"%s\"}\\n' \"${LD_LIBRARY_PATH-<unset>}\"");
      f.close();
      let chmod = new ExternalProcess;
      chmod.start("/bin/chmod 700 " + fake);
      for (; chmod.isStarting;) processEvents();
      for (; chmod.isRunning;)  processEvents();
      assert(chmod.exitCode == 0, "chmod of the fake binary failed");

      RCAstro.binaryPath = fake;
      let seen = null;
      let r = RCAstro.runCli("bxt", [], function(ev){ if (ev && ev.topic == "env") seen = ev.ldpath; });
      RCAstro.binaryPath = savedBin;
      assert(r.ok, "fake-binary run reported failure: " + r.errorMsg);
      assert(seen !== null, "fake binary never reported its environment");
      assert(seen == "<unset>", "child inherited LD_LIBRARY_PATH='" + seen + "' (must be unset)");
      W("PASS: child environment has no LD_LIBRARY_PATH");
      RCAstro.cleanup([fake]);

      // --- Case 2 (real CLI): the production entry path on a real image. ---
      // NXT is used because it is independent of the BXT/SXT model-file problem
      // documented in rcastro-runtime-investigation.md.
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let dir2 = RCAstro.tempDir();
      let inP = RCAstro.saveView(src.mainView, dir2);
      let outP = dir2 + "/out.xisf";
      RCAstro.binaryPath = null;   // real rc-astro via findBinary()
      let r2 = RCAstro.runCli("nxt", [inP, "--output", outP], null);
      assert(r2.errorMsg.indexOf("VERS_") < 0, "rc-astro linked PixInsight's onnxruntime: " + r2.errorMsg);
      assert(r2.ok, "real rc-astro nxt run failed: " + r2.errorMsg);
      assert(File.exists(outP), "real rc-astro nxt produced no output file");
      W("PASS: real rc-astro nxt ran from inside PixInsight");
      src.forceClose();
      RCAstro.cleanup([inP, outP]);

      W("PASS t_lib_env_scrub");
   } catch (e) {
      RCAstro.binaryPath = savedBin;
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
