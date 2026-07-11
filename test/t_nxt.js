// RCAstroNXT_TESTING suppresses RCAstroNXT.js's auto-invoked main() (which
// would otherwise launch the interactive NXTDialog — dialogs don't run under
// --automation-mode) so this test can call the real NXTParams.buildArgs()
// directly. See the comment at the bottom of RCAstroNXT.js.
// NOTE: RCAstroNXT.js already #include "RCAstroLib.jsh" itself — do not also
// include it here. PJSR's quoted #include silently aborts the whole script
// if the same file is #include-d twice (confirmed empirically; see task-3
// report), unlike angle-bracket <pjsr/...> system includes which tolerate it.
var RCAstroNXT_TESTING = true;
#include "../RCAstroNXT.js"

// Result goes to a FILE — console.* does not reach stdout under --automation-mode.
var RESULT_LOG = "/tmp/rc_t5_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function runTest() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let v = src.mainView;
      let before = v.image.stdDev();

      // headless path: engine round-trip with NXT args (mirrors NXTParams.buildArgs, useAdvanced=false)
      let dir = RCAstro.tempDir();
      let inP = RCAstro.saveView(v, dir);
      let outP = dir + "/o.xisf";
      let r = RCAstro.runCli("nxt", [inP, "--dn","0.90","--it","2","--fs","5.0","--device","gpu","--output",outP], null);
      assert(r.ok, "nxt failed: " + r.errorMsg);

      let out = RCAstro.importResult(outP, "nxt_out");
      RCAstro.applyInPlace(out, v);   // NOTE: applyInPlace closes `out` — do NOT forceClose it
      assert(Math.abs(v.image.stdDev() - before) > 1e-8, "applyInPlace did not modify target");

      src.forceClose();
      RCAstro.cleanup([inP, outP]);

      // --- NXTParams.buildArgs() argv verification: advanced off vs on ---
      // Call the real function (defined in RCAstroNXT.js, included above with
      // its auto-run main() suppressed) and log the argv for both shapes so
      // it can be eyeballed against the brief's flag table without a GUI.
      NXTParams.denoise = 0.90;
      NXTParams.iterations = 2;
      NXTParams.freqScale = 5.0;
      NXTParams.useAdvanced = false;
      NXTParams.mlVersion = 0;
      NXTParams.device = "gpu";
      let a1 = NXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[useAdvanced=false]: " + JSON.stringify(a1));
      assert(a1.indexOf("--dn") >= 0 && a1[a1.indexOf("--dn")+1] == "0.900", "expected --dn 0.900");
      assert(a1.indexOf("--it") >= 0 && a1[a1.indexOf("--it")+1] == "2", "expected --it 2");
      assert(a1.indexOf("--fs") >= 0 && a1[a1.indexOf("--fs")+1] == "5.00", "expected --fs 5.00");
      assert(a1.indexOf("--di") < 0 && a1.indexOf("--dc") < 0 && a1.indexOf("--dhf") < 0 && a1.indexOf("--dlf") < 0 &&
             a1.indexOf("--dihf") < 0 && a1.indexOf("--dilf") < 0 && a1.indexOf("--dchf") < 0 && a1.indexOf("--dclf") < 0,
             "expected no advanced flags when useAdvanced=false");
      assert(a1.indexOf("--ml-version") < 0, "expected no --ml-version when mlVersion=0 (Latest)");
      assert(a1.indexOf("--device") >= 0 && a1[a1.indexOf("--device")+1] == "gpu", "expected --device gpu");
      assert(a1[a1.length-2] == "--output" && a1[a1.length-1] == "OUT.xisf", "expected --output OUT.xisf at the end");

      NXTParams.useAdvanced = true;
      NXTParams.intensity = 0.5; NXTParams.color = 0.6;
      NXTParams.dhf = 0.7; NXTParams.dlf = 0.8; NXTParams.dihf = 0.1; NXTParams.dilf = 0.2;
      NXTParams.dchf = 0.3; NXTParams.dclf = 0.4;
      let a2 = NXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[useAdvanced=true]: " + JSON.stringify(a2));
      assert(a2.indexOf("--di") >= 0 && a2[a2.indexOf("--di")+1] == "0.500", "expected --di 0.500");
      assert(a2.indexOf("--dc") >= 0 && a2[a2.indexOf("--dc")+1] == "0.600", "expected --dc 0.600");
      assert(a2.indexOf("--dhf") >= 0 && a2[a2.indexOf("--dhf")+1] == "0.700", "expected --dhf 0.700");
      assert(a2.indexOf("--dlf") >= 0 && a2[a2.indexOf("--dlf")+1] == "0.800", "expected --dlf 0.800");
      assert(a2.indexOf("--dihf") >= 0 && a2[a2.indexOf("--dihf")+1] == "0.100", "expected --dihf 0.100");
      assert(a2.indexOf("--dilf") >= 0 && a2[a2.indexOf("--dilf")+1] == "0.200", "expected --dilf 0.200");
      assert(a2.indexOf("--dchf") >= 0 && a2[a2.indexOf("--dchf")+1] == "0.300", "expected --dchf 0.300");
      assert(a2.indexOf("--dclf") >= 0 && a2[a2.indexOf("--dclf")+1] == "0.400", "expected --dclf 0.400");

      NXTParams.mlVersion = 3;
      let a3 = NXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[mlVersion=3]: " + JSON.stringify(a3));
      assert(a3.indexOf("--ml-version") >= 0 && a3[a3.indexOf("--ml-version")+1] == "3", "expected --ml-version 3");

      W("PASS t_nxt");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
runTest();
