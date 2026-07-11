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
      let deviceInfo = null;
      let r = RCAstro.runCli("bxt",
         [inP, "--ss", "0.25", "--sn", "0.90", "--device", "gpu", "--output", outP],
         function(ev){
            if (ev && ev.event == "progress") sawProgress = true;
            // Observed shape: {"event":"info","topic":"device","device":"gpu",
            // "id":"gpu","name":"NVIDIA GeForce RTX 5070 Ti","provider":"CUDA",
            // "runtime":"onnxruntime 1.23.2"}
            if (ev && ev.event == "info" && ev.topic == "device") deviceInfo = ev;
         });

      assert(r.ok, "runCli reported failure: " + r.errorMsg);
      assert(File.exists(outP), "no output file produced");

      let out = RCAstro.importResult(outP, "cli_out");
      // output must differ from input (deconvolution changed pixels)
      assert(Math.abs(out.mainView.image.stdDev() - srcStdDev) > 1e-9, "output identical to input");
      W("progress events seen: " + sawProgress);

      // Finding 3: GPU selection must be asserted from a real device-info event,
      // not just trusted because we passed --device gpu on the command line.
      assert(deviceInfo != null, "no device info event observed from rc-astro");
      assert(deviceInfo.device == "gpu", "rc-astro did not report using the gpu device (got: " + deviceInfo.device + ")");
      W("device used: " + deviceInfo.device + " (" + (deviceInfo.name || "unknown name") + ")");

      src.forceClose(); out.forceClose();
      RCAstro.cleanup([inP, outP]);

      // Finding 2/1: negative-path — a genuine rc-astro failure with no JSON
      // error event at all (it aborts before ever getting there), so the only
      // way to get an informative errorMsg is the stderr-fallback added for
      // Finding 1. A corrupt/non-XISF input file reliably reproduces this
      // (verified manually: rc-astro core-dumps with
      // "terminate called after throwing an instance of 'rcastro::Error' ...
      // is not a valid XISF file (bad signature)" on stderr and NO JSON error
      // line on stdout).
      let dir2 = RCAstro.tempDir();
      let badInP = dir2 + "/not_really_xisf.xisf";
      let badOutP = dir2 + "/out.xisf";
      let junk = new File;
      junk.createForWriting(badInP);
      junk.outTextLn("not an image");
      junk.close();

      let r2 = RCAstro.runCli("bxt",
         [badInP, "--device", "gpu", "--output", badOutP],
         null);

      assert(r2.ok === false, "expected runCli to report failure for a corrupt input file");
      assert(typeof r2.errorMsg == "string" && r2.errorMsg.trim().length > 0, "errorMsg must be a non-empty string on failure");
      W("negative-path errorMsg: " + r2.errorMsg);

      RCAstro.cleanup([badInP, badOutP]);

      W("PASS t_lib_runcli");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
