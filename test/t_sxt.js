// RCAstroSXT_TESTING suppresses RCAstroSXT.js's auto-invoked main() (which
// would otherwise launch the interactive SXTDialog — dialogs don't run under
// --automation-mode) so this test can call the real runSXT()/SXTParams
// directly. See the comment at the bottom of RCAstroSXT.js.
// NOTE: RCAstroSXT.js already #include "RCAstroLib.jsh" itself — do not also
// include it here. PJSR's quoted #include silently aborts the whole script
// if the same file is #include-d twice (confirmed empirically; see task-3
// report), unlike angle-bracket <pjsr/...> system includes which tolerate it.
var RCAstroSXT_TESTING = true;
#include "../RCAstroSXT.js"

// Result goes to a FILE — console.* does not reach stdout under --automation-mode.
var RESULT_LOG = "/tmp/rc_t4_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      // --- Part 1: raw engine round-trip, DISCOVERING the real stars filename ---
      // (Documents the empirical evidence backing the "-stars.xisf" name baked
      // into runSXT() below — do not trust any assumed name, read it from disk.)
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let v = src.mainView; let id = v.id;
      let beforeStdDev = v.image.stdDev();

      let dir = RCAstro.tempDir();
      let inP = RCAstro.saveView(v, dir);
      let outP = dir + "/" + id + "_starless.xisf";
      // CONFIRMED via empirical run: CLI writes "<outP-without-.xisf>-stars.xisf"
      // (hyphen, not underscore) — see wrote: lines below.
      let starsP = dir + "/" + id + "_starless-stars.xisf";

      let r = RCAstro.runCli("sxt", [inP, "--output-stars", "--device","gpu","--output",outP], null);
      assert(r.ok, "sxt failed: " + r.errorMsg);
      assert(File.exists(outP), "no starless output");

      // Log EVERYTHING the CLI actually wrote.
      // NOTE: bare global searchDirectory() — File.searchDirectory() is NOT callable in this PI build.
      let found = searchDirectory(dir + "/*.xisf");
      for (let i = 0; i < found.length; ++i) W("wrote: " + found[i]);

      assert(File.exists(starsP), "no stars output at discovered path " + starsP);
      RCAstro.cleanup([inP, outP, starsP]);

      // --- Part 2: real runSXT() end-to-end — starless+stars to NEW windows, original untouched ---
      SXTParams.targetView = v;
      SXTParams.outputStars = true;
      SXTParams.unscreen = false;
      SXTParams.mlVersion = 0;
      SXTParams.device = "gpu";

      runSXT(v);

      assert(!v.isNull, "original view was closed by runSXT (must be preserved)");
      assert(Math.abs(v.image.stdDev() - beforeStdDev) < 1e-9,
             "original view pixels changed — runSXT must NOT apply in place");

      let starlessWin = ImageWindow.windowById(id + "_starless");
      assert(!starlessWin.isNull, "no " + id + "_starless window created");
      let starsWin = ImageWindow.windowById(id + "_stars");
      assert(!starsWin.isNull, "no " + id + "_stars window created");

      starlessWin.forceClose();
      starsWin.forceClose();
      src.forceClose();

      // --- Part 3: buildArgs() argv checks ---
      SXTParams.outputStars = false;
      SXTParams.unscreen = false;
      SXTParams.mlVersion = 0;
      SXTParams.device = "gpu";
      let a1 = SXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[outputStars=false]: " + JSON.stringify(a1));
      assert(a1.indexOf("--output-stars") < 0, "expected no --output-stars when outputStars=false");
      assert(a1.indexOf("--unscreen") < 0, "expected no --unscreen when outputStars=false");
      assert(a1.indexOf("--ml-version") < 0, "expected no --ml-version when mlVersion=0 (Latest)");

      SXTParams.outputStars = true;
      SXTParams.unscreen = true;
      let a2 = SXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[outputStars=true, unscreen=true]: " + JSON.stringify(a2));
      assert(a2.indexOf("--output-stars") >= 0, "expected --output-stars when outputStars=true");
      assert(a2.indexOf("--unscreen") >= 0, "expected --unscreen when unscreen=true and outputStars=true");

      SXTParams.mlVersion = 11;
      let a3 = SXTParams.buildArgs("IN.xisf", "OUT.xisf");
      W("argv[mlVersion=11]: " + JSON.stringify(a3));
      assert(a3.indexOf("--ml-version") >= 0 && a3[a3.indexOf("--ml-version")+1] == "11", "expected --ml-version 11");

      W("PASS t_sxt");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
