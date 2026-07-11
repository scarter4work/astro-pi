#include "../RCAstroLib.jsh"
// NOTE: console.* output does not reach process stdout under
// `PixInsight --automation-mode` (confirmed empirically for this PI 1.9.4
// install: even a bare console.noteln() smoke test produced nothing on
// stdout). So pass/fail is additionally logged to a file that the harness
// can grep reliably. console.* calls are kept for interactive debugging.
var RESULT_LOG = "/tmp/rc_t1_result.log";

function assert(c, m){ if(!c){ throw new Error(m);} }

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      // findBinary resolves
      let bin = RCAstro.findBinary();
      assert(bin != null, "rc-astro binary not found");
      W("binary: " + bin);

      // load a known image, save via engine, re-import, compare geometry
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let dir = RCAstro.tempDir();
      let p   = RCAstro.saveView(src.mainView, dir);
      assert(File.exists(p), "saveView did not write file");

      let rt  = RCAstro.importResult(p, "rt_check");
      assert(rt.mainView.image.width  == src.mainView.image.width,  "width mismatch");
      assert(rt.mainView.image.height == src.mainView.image.height, "height mismatch");

      src.forceClose(); rt.forceClose();
      RCAstro.cleanup([p]);
      W("PASS t_lib_roundtrip");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
