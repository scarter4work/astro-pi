#include "../RCAstroLib.jsh"
// Result goes to a FILE — console.* does not reach stdout under --automation-mode.
// Same pattern as test/t_lib_roundtrip.js (Task 1).
var RESULT_LOG = "/tmp/rc_t2_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let dir = RCAstro.tempDir();
      let inP = RCAstro.saveView(src.mainView, dir);
      let outP = dir + "/out.xisf";
      let srcStdDev = src.mainView.image.stdDev();

      let sawProgress = false;
      let r = RCAstro.runCli("bxt",
         [inP, "--ss", "0.25", "--sn", "0.90", "--device", "gpu", "--output", outP],
         function(ev){ if (ev && ev.event == "progress") sawProgress = true; });

      assert(r.ok, "runCli reported failure: " + r.errorMsg);
      assert(File.exists(outP), "no output file produced");

      let out = RCAstro.importResult(outP, "cli_out");
      // output must differ from input (deconvolution changed pixels)
      assert(Math.abs(out.mainView.image.stdDev() - srcStdDev) > 1e-9, "output identical to input");
      W("progress events seen: " + sawProgress);

      src.forceClose(); out.forceClose();
      RCAstro.cleanup([inP, outP]);
      W("PASS t_lib_runcli");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
