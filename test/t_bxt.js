// RCAstroBXT_TESTING suppresses RCAstroBXT.js's auto-invoked main() (which
// would otherwise launch the interactive BXTDialog — dialogs don't run under
// --automation-mode) so this test can call the real runBXT()/BXTParams
// directly. See the comment at the bottom of RCAstroBXT.js.
// NOTE: RCAstroBXT.js already #include "RCAstroLib.jsh" itself — do not also
// include it here. PJSR's quoted #include silently aborts the whole script
// if the same file is #include-d twice (confirmed empirically; see task-3
// report), unlike angle-bracket <pjsr/...> system includes which tolerate it.
var RCAstroBXT_TESTING = true;
#include "../RCAstroBXT.js"

// Result goes to a FILE — console.* does not reach stdout under --automation-mode.
var RESULT_LOG = "/tmp/rc_t3_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function runTest() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let v = src.mainView;
      let before = v.image.stdDev();

      // headless path: call the REAL production entry point runBXT(), not a
      // reimplementation, so the actual shipped glue (BXTParams -> buildArgs
      // -> runCli -> the !r.ok fail branch -> the finally cleanup) is
      // genuinely exercised. This is what would have caught Fix B (temp dir
      // stranded when saveView() throws before the try).
      BXTParams.targetView = v;
      BXTParams.sharpenStars = 0.25;
      BXTParams.sharpenNonstellar = 0.90;
      BXTParams.adjustHalos = 0.0;
      BXTParams.autoPSF = true;
      BXTParams.psfRadius = 0.0;
      BXTParams.correctOnly = false;
      BXTParams.mlVersion = 0;
      BXTParams.device = "gpu";

      runBXT(v);
      assert(Math.abs(v.image.stdDev() - before) > 1e-8, "runBXT did not modify target view in place");

      src.forceClose();

      // --- BXTParams.buildArgs() argv verification (per task-3-brief table) ---
      // Call the real function (defined in RCAstroBXT.js, included above with
      // its auto-run main() suppressed) and log the argv for a few
      // representative combinations so it can be eyeballed against the brief's
      // flag table without a GUI.
      BXTParams.sharpenStars = 0.25;
      BXTParams.sharpenNonstellar = 0.90;
      BXTParams.adjustHalos = 0.10;
      BXTParams.autoPSF = true;
      BXTParams.psfRadius = 0.0;
      BXTParams.correctOnly = false;
      BXTParams.mlVersion = 0;
      BXTParams.device = "gpu";
      let a1 = BXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[default ss/sn, autoPSF=true]: " + JSON.stringify(a1));
      assert(a1.indexOf("--ss") >= 0 && a1.indexOf("--sn") >= 0, "expected --ss/--sn present when correctOnly=false");
      assert(a1.indexOf("--ansr") >= 0, "expected --ansr present when autoPSF=true");
      assert(a1.indexOf("--no-ansr") < 0 && a1.indexOf("--nsr") < 0, "expected no --no-ansr/--nsr when autoPSF=true");
      assert(a1.indexOf("--correct-only") < 0, "expected no --correct-only when correctOnly=false");
      assert(a1.indexOf("--ml-version") < 0, "expected no --ml-version when mlVersion=0 (Latest)");

      BXTParams.autoPSF = false;
      BXTParams.psfRadius = 1.5;
      let a2 = BXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[autoPSF=false, psfRadius=1.5]: " + JSON.stringify(a2));
      assert(a2.indexOf("--no-ansr") >= 0, "expected --no-ansr when autoPSF=false");
      assert(a2.indexOf("--nsr") >= 0 && a2[a2.indexOf("--nsr")+1] == "1.50", "expected --nsr 1.50 when autoPSF=false");
      assert(a2.indexOf("--ansr") < 0, "expected plain --ansr NOT present when autoPSF=false");

      BXTParams.correctOnly = true;
      let a3 = BXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[correctOnly=true]: " + JSON.stringify(a3));
      assert(a3.indexOf("--correct-only") >= 0, "expected --correct-only when correctOnly=true");
      assert(a3.indexOf("--ss") < 0 && a3.indexOf("--sn") < 0, "expected --correct-only to suppress --ss/--sn");
      // Fix G: --ash is invalid together with --correct-only (verified against
      // a real `rc-astro bxt --correct-only --ash 0.100` invocation --
      // {"event":"error","message":"--correct-only forces --ash to 0, which
      // conflicts with the value you gave; omit --ash"}). Must never be
      // emitted in this mode, even when adjustHalos is non-zero.
      assert(a3.indexOf("--ash") < 0, "expected no --ash when correctOnly=true (CLI rejects --ash with --correct-only)");

      BXTParams.correctOnly = false;
      BXTParams.autoPSF = true;
      BXTParams.mlVersion = 4;
      let a4 = BXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[mlVersion=4]: " + JSON.stringify(a4));
      assert(a4.indexOf("--ml-version") >= 0 && a4[a4.indexOf("--ml-version")+1] == "4", "expected --ml-version 4");

      W("PASS t_bxt");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
runTest();
