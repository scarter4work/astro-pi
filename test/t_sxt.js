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

      // --- Part 2b: Fix H — re-run on the SAME view must not collide with
      // the windows the first run just created (deterministic "<id>_starless"
      // / "<id>_stars" ids). Before the fix, PI's own id-uniquify-on-open
      // (best case "_starless1" pile-up) or the newWindow() id= re-assignment
      // onto the still-taken id (worst case: throws after the ~2.7s CLI run)
      // would have broken this.
      runSXT(v);
      let starlessWin2 = ImageWindow.windowById(id + "_starless");
      let starsWin2 = ImageWindow.windowById(id + "_stars");
      assert(!starlessWin2.isNull, "re-run did not (re)create " + id + "_starless");
      assert(!starsWin2.isNull, "re-run did not (re)create " + id + "_stars");
      assert(ImageWindow.windowById(id + "_starless1").isNull,
             "found a stray uniquified '" + id + "_starless1' window — Fix H collision not handled");
      assert(ImageWindow.windowById(id + "_stars1").isNull,
             "found a stray uniquified '" + id + "_stars1' window — Fix H collision not handled");

      starlessWin2.forceClose();
      starsWin2.forceClose();
      src.forceClose();

      // --- Part 2c: Fix D — SXT's new windows must preserve the source's
      // astrometric solution. The gxp panel used above has none to begin
      // with (verified: hasAstrometricSolution=false, 0 FITS keywords), so
      // it can't exercise this. Use a real plate-solved/registered frame
      // instead (confirmed via probe: hasAstrometricSolution=true).
      let srcSolved = ImageWindow.open(
         "/home/scarter4work/astro_work/ic4604/registered/Light_IC 4604_2-4_60.0s_Bin1_Lqef_20260616-001157_181deg_0001_d_r.xisf")[0];
      let vSolved = srcSolved.mainView; let idSolved = vSolved.id;
      assert(srcSolved.hasAstrometricSolution, "test fixture unexpectedly has no astrometric solution");

      SXTParams.targetView = vSolved;
      SXTParams.outputStars = true;
      SXTParams.unscreen = false;
      SXTParams.mlVersion = 0;
      SXTParams.device = "gpu";
      runSXT(vSolved);

      let starlessSolved = ImageWindow.windowById(idSolved + "_starless");
      let starsSolved = ImageWindow.windowById(idSolved + "_stars");
      assert(!starlessSolved.isNull, "no " + idSolved + "_starless window created");
      assert(!starsSolved.isNull, "no " + idSolved + "_stars window created");
      assert(starlessSolved.hasAstrometricSolution,
             "Fix D regressed: " + idSolved + "_starless has no astrometric solution");
      assert(starsSolved.hasAstrometricSolution,
             "Fix D regressed: " + idSolved + "_stars has no astrometric solution");

      starlessSolved.forceClose();
      starsSolved.forceClose();
      srcSolved.forceClose();

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
