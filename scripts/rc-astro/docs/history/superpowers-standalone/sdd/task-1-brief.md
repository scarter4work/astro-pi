### Task 1: Engine foundation + headless test harness

**Files:**
- Create: `~/PixInsightScripts/RC-Astro/RCAstroLib.jsh`
- Create: `~/PixInsightScripts/RC-Astro/test/run-headless.sh`
- Test: `~/PixInsightScripts/RC-Astro/test/t_lib_roundtrip.js`

**Interfaces:**
- Produces (used by all later tasks):
  - `RCAstro.TITLE : String`
  - `RCAstro.findBinary() -> String|null` (caches `RCAstro.binaryPath`)
  - `RCAstro.tempDir() -> String` (creates a unique subdir under TMPDIR)
  - `RCAstro.saveView(view, dir) -> String` (writes `<dir>/<view.id>.xisf`, returns path)
  - `RCAstro.importResult(path, id) -> ImageWindow` (opens hidden; `id` optional rename)
  - `RCAstro.applyInPlace(resultWindow, targetView) -> void`
  - `RCAstro.newWindow(resultWindow, id) -> ImageWindow`
  - `RCAstro.cleanup(pathsArray) -> void`
  - `RCAstro.fail(message) -> void`

- [ ] **Step 1: Write the headless runner**

Create `test/run-headless.sh`:

```bash
#!/usr/bin/env bash
# Xvfb + PixInsight headless PJSR runner. Usage: run-headless.sh path/to/test.js
set -euo pipefail
dirname=/opt/PixInsight/bin
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$dirname/lib:$dirname
export CUDA_VISIBLE_DEVICES=0
export TF_CPP_MIN_LOG_LEVEL=1
export LC_ALL=en_US.utf8
export QT_PLUGIN_PATH=$dirname/lib/qt-plugins
export QT_QPA_PLATFORM_PLUGIN_PATH=$dirname/lib/qt-plugins/platforms
export QT_QPA_PLATFORM=xcb
export QT_LOGGING_RULES='*=false'
export AVAHI_COMPAT_NOWARN=1
export TMPDIR=/home/scarter4work/pixinsight-swap
mkdir -p "$TMPDIR"
script="$1"
xvfb-run -a -s "-screen 0 1920x1080x24" \
  /opt/PixInsight/bin/PixInsight --new --automation-mode --force-exit -r="$script"
```

Then `chmod +x test/run-headless.sh`.

- [ ] **Step 2: Write the failing engine round-trip test**

Create `test/t_lib_roundtrip.js`:

```javascript
#include "../RCAstroLib.jsh"
function assert(c, m){ if(!c){ console.criticalln("FAIL: "+m); throw new Error(m);} }

function main() {
   // findBinary resolves
   let bin = RCAstro.findBinary();
   assert(bin != null, "rc-astro binary not found");
   console.noteln("binary: " + bin);

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
   console.noteln("PASS t_lib_roundtrip");
}
main();
```

- [ ] **Step 3: Run it, verify it fails**

Run: `test/run-headless.sh "$PWD/test/t_lib_roundtrip.js" 2>&1 | tee /tmp/rc_t1.log; grep -q "PASS t_lib_roundtrip" /tmp/rc_t1.log && echo OK || echo FAILED`
Expected: `FAILED` (RCAstroLib.jsh does not exist yet → include error).

- [ ] **Step 4: Implement `RCAstroLib.jsh`**

Create `RCAstroLib.jsh`:

```javascript
// RC-Astro CLI engine — shared by RCAstro{BXT,SXT,NXT}.js
#include <pjsr/UndoFlag.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/StdButton.jsh>

var RCAstro = {
   TITLE: "RC-Astro CLI",
   binaryPath: null,

   findBinary: function() {
      if (this.binaryPath && File.exists(this.binaryPath)) return this.binaryPath;
      let candidates = ["/usr/local/bin/rc-astro"];
      // resolve via PATH
      let p = new ExternalProcess;
      p.start("/usr/bin/which rc-astro");
      for (; p.isStarting;) CoreApplication.processEvents();
      for (; p.isRunning;)  CoreApplication.processEvents();
      let which = String(p.stdout).trim();
      if (which.length > 0) candidates.push(which);
      for (let i = 0; i < candidates.length; ++i)
         if (candidates[i] && File.exists(candidates[i])) { this.binaryPath = candidates[i]; return candidates[i]; }
      return null;
   },

   tempDir: function() {
      let base = (typeof(getEnvironmentVariable) != 'undefined' && getEnvironmentVariable("TMPDIR"))
                 ? getEnvironmentVariable("TMPDIR") : File.systemTempDirectory;
      let dir = base + "/rc-astro-" + Math.trunc(Date.now()) + "-" + Math.trunc(Math.random()*1e6);
      if (!File.directoryExists(dir)) File.createDirectory(dir, true);
      return dir;
   },

   saveView: function(view, dir) {
      let win = view.isMainView ? view.window : view.mainView.window;
      let path = dir + "/" + view.id + ".xisf";
      // saveAs(path, queryOptions=false, allowMessages=false, strict=true, verifyOverwrite=false)
      if (!win.saveAs(path, false, false, true, false))
         this.fail("Could not write temp image: " + path);
      return path;
   },

   importResult: function(path, id) {
      if (!File.exists(path)) this.fail("Expected output not found: " + path);
      let wins = (id && id.length) ? ImageWindow.open(path, id, "", true)
                                   : ImageWindow.open(path);
      if (!wins || wins.length == 0) this.fail("Could not open output: " + path);
      return wins[0];
   },

   // Replace targetView's pixels with resultWindow's image, single undoable step.
   applyInPlace: function(resultWindow, targetView) {
      let P = new PixelMath;
      P.expression = resultWindow.mainView.id;
      P.useSingleExpression = true;
      P.generateOutput = true;
      P.createNewImage = false;
      P.rescale = false;
      P.truncate = false;
      P.newImageColorSpace = PixelMath.SameAsTarget;
      P.newImageSampleFormat = PixelMath.SameAsTarget;
      P.executeOn(targetView);
   },

   newWindow: function(resultWindow, id) {
      resultWindow.mainView.id = id;
      resultWindow.show();
      resultWindow.zoomToFit();
      return resultWindow;
   },

   cleanup: function(paths) {
      for (let i = 0; i < paths.length; ++i)
         try { if (paths[i] && File.exists(paths[i])) File.remove(paths[i]); } catch (e) {}
   },

   fail: function(message) {
      console.criticalln("*** " + this.TITLE + ": " + message);
      (new MessageBox("<p>" + message + "</p>", this.TITLE, StdIcon.Error, StdButton.Ok)).execute();
      throw new Error(message);
   }
};
```

- [ ] **Step 5: Run it, verify it passes**

Run: `test/run-headless.sh "$PWD/test/t_lib_roundtrip.js" 2>&1 | tee /tmp/rc_t1.log; grep -q "PASS t_lib_roundtrip" /tmp/rc_t1.log && echo OK || echo FAILED`
Expected: `OK`

- [ ] **Step 6: Commit**

```bash
cd ~/PixInsightScripts/RC-Astro
git add RCAstroLib.jsh test/run-headless.sh test/t_lib_roundtrip.js
git commit -m "feat: RC-Astro engine foundation + headless round-trip test"
```

---

