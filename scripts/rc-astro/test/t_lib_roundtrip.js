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

   let p = null;
   let dir = null;
   let src = null;
   let rt = null;
   try {
      // findBinary resolves
      let bin = RCAstro.findBinary();
      assert(bin != null, "rc-astro binary not found");
      W("binary: " + bin);

      // load a known image, save via engine, re-import, compare geometry
      src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      dir = RCAstro.tempDir();
      p = RCAstro.saveView(src.mainView, dir);
      assert(File.exists(p), "saveView did not write file");

      rt = RCAstro.importResult(p, "rt_check");
      assert(rt.mainView.image.width  == src.mainView.image.width,  "width mismatch");
      assert(rt.mainView.image.height == src.mainView.image.height, "height mismatch");

      src.forceClose(); src = null;
      rt.forceClose(); rt = null;
      RCAstro.cleanup([p]);
      assert(!File.exists(p), "cleanup did not remove temp file");
      assert(!File.directoryExists(dir), "cleanup did not remove temp directory");
      p = null;
      W("PASS t_lib_roundtrip");
   } catch (e) {
      W("FAIL: " + e.message);
   } finally {
      // Ensure any still-open windows are closed even if an assertion
      // above failed before the normal close/cleanup path ran.
      if (src != null) try { src.forceClose(); } catch (e2) {}
      if (rt  != null) try { rt.forceClose();  } catch (e2) {}
      // Ensure the temp file (and its temp dir) never leak, even if an
      // assertion above failed before RCAstro.cleanup() ran.
      if (p != null) try { RCAstro.cleanup([p]); } catch (e2) {}
   }
   log.close();
}
main();
