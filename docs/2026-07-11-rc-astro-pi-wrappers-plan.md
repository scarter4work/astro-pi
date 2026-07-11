# RC-Astro CLI wrappers for PixInsight — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add three PixInsight Scripts-menu tools (BlurXTerminator / StarXTerminator / NoiseXTerminator, "CLI") that run the GPU-accelerated `rc-astro` binary via PJSR, with full dialogs, in-place/undo output, process-icon automation, and loud error handling.

**Architecture:** A shared engine (`RCAstroLib.jsh`) owns binary discovery, view↔XISF round-trip, `ExternalProcess` invocation with `--json` NDJSON parsing, result application (PixelMath in-place, or new window), Parameters helpers, and error surfacing. Three thin tool scripts (`RCAstro{BXT,SXT,NXT}.js`) each define a parameter object, a `Dialog` subclass, and a `main()` that dispatches interactive vs. headless (`Parameters`) execution.

**Tech Stack:** PixInsight PJSR (JavaScript), `rc-astro` CLI v0.9.10 (ONNX Runtime, GPU), XISF I/O, Xvfb for headless tests.

## Global Constraints

- Binary path: prefer `/usr/local/bin/rc-astro`, else resolve via `PATH`. Never hard-fail silently if missing — show a `MessageBox`.
- CLI invocation always includes `--json --overwrite` and an explicit `--device` (`gpu` default, `cpu` option). Model: omit `--ml-version` for "Latest", else pass the integer.
- Output rules: **BXT & NXT** replace the target view in place (single undoable PixelMath step). **SXT** writes `<id>_starless` to a new window (original preserved); `<id>_stars` to another new window only when stars output is enabled.
- Errors are loud and real: missing binary, nonzero exit, JSON `error` event, license failure, or unreadable output → `RCAstro.fail(<real message>)`. No mock data, no silent CPU fallback beyond the CLI's own (which is echoed to the console).
- Temp files live under `TMPDIR` (`~/pixinsight-swap`, NVMe) in a per-run unique subdir, and are always cleaned up (including on failure).
- Platform is Linux only. Menu names end with " (CLI)". Scripts install to `~/PixInsightScripts/RC-Astro/` and register via Feature Scripts.
- Reference idioms (verified working on this box): `/opt/PixInsight/src/scripts/Toolbox/GraXpertLib.jsh` (ExternalProcess, PixelMath assign, saveAs, ImageWindow.open) and `SetiAStroCosmicClarityDenoise.js` (Parameters save/load, dialog controls).

**Environment facts established in Task 1 — all later tasks MUST follow these:**
- Use the bare global **`processEvents()`**, NOT `CoreApplication.processEvents()` (the latter does not exist in this PI 1.9.4 build).
- Use the bare global **`searchDirectory(pattern)`**, NOT `File.searchDirectory(...)` (documented but not callable in this build). It returns full paths.
- **`console.*` output does NOT reach stdout under `PixInsight --automation-mode`.** Every headless test therefore writes its verdict to a result file and the runner greps *that file*, following the exact pattern established in `test/t_lib_roundtrip.js` (a `RESULT_LOG` path, a `W()` helper writing `log.outTextLn(...)` + `flush()`, and `try/catch` writing `PASS <name>` or `FAIL: <msg>`). Copy that pattern; do not grep stdout.
- **`RCAstro.applyInPlace(resultWindow, targetView)` takes ownership of `resultWindow` and closes it.** Callers must NOT call `forceClose()` on it afterward (double-close).
- `RCAstro.cleanup(paths)` removes the given files AND the per-run temp directories created by `RCAstro.tempDir()`.

---

## File Structure

```
~/PixInsightScripts/RC-Astro/
  RCAstroLib.jsh       shared engine (Task 1 + Task 2)
  RCAstroBXT.js        BlurXTerminator tool  (Task 3)
  RCAstroSXT.js        StarXTerminator tool  (Task 4)
  RCAstroNXT.js        NoiseXTerminator tool (Task 5)
  test/
    run-headless.sh    Xvfb + PixInsight -r runner (Task 1)
    t_lib_roundtrip.js engine round-trip test    (Task 1)
    t_lib_runcli.js    engine CLI-run test        (Task 2)
    t_bxt.js           BXT headless smoke test    (Task 3)
    t_sxt.js           SXT headless smoke test    (Task 4)
    t_nxt.js           NXT headless smoke test    (Task 5)
  install.sh           copy + Feature Scripts guidance (Task 6)
  README.md            usage + install (Task 6)
```

**Shared test image:** `~/astro_work/cygnus/gxp/panel_1-1.xisf` (verified linear, 22 MP, GPU-processes in ~2.7 s).

---

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

### Task 2: Engine CLI invocation (`runCli`) with NDJSON parsing

**Files:**
- Modify: `~/PixInsightScripts/RC-Astro/RCAstroLib.jsh` (add `runCli`)
- Test: `~/PixInsightScripts/RC-Astro/test/t_lib_runcli.js`

**Interfaces:**
- Consumes: `RCAstro.findBinary`, `RCAstro.fail` (Task 1)
- Produces: `RCAstro.runCli(tool, argsArray, onEvent) -> {exit:Number, ok:Boolean, errorMsg:String}`
  - `argsArray` are already-quoted-safe tokens (caller passes plain strings; runCli quotes paths it builds). `onEvent(obj)` receives each parsed NDJSON object (may be null-tolerant).

- [ ] **Step 1: Write the failing CLI-run test**

Create `test/t_lib_runcli.js`:

```javascript
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
```

- [ ] **Step 2: Run it, verify it fails**

Run: `rm -f /tmp/rc_t2_result.log; test/run-headless.sh "$PWD/test/t_lib_runcli.js" >/dev/null 2>&1; grep -q "PASS t_lib_runcli" /tmp/rc_t2_result.log 2>/dev/null && echo OK || { echo FAILED; cat /tmp/rc_t2_result.log 2>/dev/null; }`
Expected: `FAILED` (`runCli` undefined).

- [ ] **Step 3: Implement `runCli`**

Add to the `RCAstro` object in `RCAstroLib.jsh` (before the closing `}` of the object, after `fail` — add a comma after `fail`'s function):

```javascript
   ,
   // Run rc-astro <tool> with args; parse --json NDJSON stream via onEvent.
   runCli: function(tool, argsArray, onEvent) {
      let bin = this.findBinary();
      if (!bin) this.fail("rc-astro CLI not found at /usr/local/bin/rc-astro or on PATH.");

      // Build a quoted command line: quote tokens that are paths / contain spaces.
      let quote = function(s){ return /[^A-Za-z0-9_.\-\/]/.test(s) ? '"' + s + '"' : s; };
      let parts = [quote(bin), "--no-banner", tool];
      for (let i = 0; i < argsArray.length; ++i) parts.push(quote(String(argsArray[i])));
      parts.push("--json", "--overwrite");
      let cmdLine = parts.join(" ");
      console.writeln("running: " + cmdLine);

      let errorMsg = "";
      let buffer   = "";
      let dispatch = function(line) {
         line = line.trim();
         if (line.length == 0 || line.charAt(0) != "{") { if (line.length) console.writeln(line); return; }
         let obj = null;
         try { obj = JSON.parse(line); } catch (e) { return; }
         if (obj) {
            if (obj.event == "error")   { errorMsg = obj.message || obj.text || JSON.stringify(obj); console.criticalln("rc-astro: " + errorMsg); }
            else if (obj.event == "warning") console.warningln("rc-astro: " + (obj.message || obj.text || ""));
            if (onEvent) onEvent(obj);
         }
      };

      let p = new ExternalProcess;
      p.onStandardOutputDataAvailable = function() {
         buffer += String(this.stdout);
         let nl;
         while ((nl = buffer.indexOf("\n")) >= 0) { dispatch(buffer.substring(0, nl)); buffer = buffer.substring(nl + 1); }
      };
      p.onStandardErrorDataAvailable = function() {
         let e = String(this.stderr).trim();
         if (e.length) console.warningln("rc-astro[stderr]: " + e);
      };

      try {
         p.start(cmdLine);
         for (; p.isStarting;) CoreApplication.processEvents();
         for (; p.isRunning;)  CoreApplication.processEvents();
      } catch (e) {
         this.fail("Failed to launch rc-astro: " + e.message);
      }
      if (buffer.length) dispatch(buffer);

      let exit = p.exitCode;
      let ok = (exit == 0) && (errorMsg.length == 0);
      if (!ok && errorMsg.length == 0) errorMsg = "rc-astro exited with code " + exit;
      return { exit: exit, ok: ok, errorMsg: errorMsg };
   }
```

(Also remove the now-inner `throw` reliance: `runCli` returns a result object; callers decide whether to `fail`. Note `fail` is still called for unrecoverable launch/binary problems.)

- [ ] **Step 4: Run it, verify it passes**

Run: `rm -f /tmp/rc_t2_result.log; test/run-headless.sh "$PWD/test/t_lib_runcli.js" 2>&1 | tee /tmp/rc_t2_console.log >/dev/null; grep -q "PASS t_lib_runcli" /tmp/rc_t2_result.log && echo OK || { echo FAILED; cat /tmp/rc_t2_result.log; }`
Expected: `OK`. Also confirm GPU was used: `grep -i "Using gpu" /tmp/rc_t2_console.log` should show `NVIDIA GeForce RTX 5070 Ti` (rc-astro's own stdout is echoed through the PJSR console handler).

- [ ] **Step 5: Commit**

```bash
cd ~/PixInsightScripts/RC-Astro
git add RCAstroLib.jsh test/t_lib_runcli.js
git commit -m "feat: runCli with NDJSON parsing + GPU CLI integration test"
```

---

### Task 3: BlurXTerminator tool (`RCAstroBXT.js`)

**Files:**
- Create: `~/PixInsightScripts/RC-Astro/RCAstroBXT.js`
- Test: `~/PixInsightScripts/RC-Astro/test/t_bxt.js`

**Interfaces:**
- Consumes: full `RCAstro` engine (Tasks 1–2).
- Produces: a `#feature-id` script; global `BXTParams` with `.save()/.load()/.buildArgs(inPath,outPath)`; `main()` handling `Parameters.isViewTarget` / `isGlobalTarget` / interactive.

- [ ] **Step 1: Write the failing headless smoke test**

Create `test/t_bxt.js`:

```javascript
#include "../RCAstroLib.jsh"
// Result goes to a FILE — console.* does not reach stdout under --automation-mode.
var RESULT_LOG = "/tmp/rc_t3_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let v = src.mainView;
      let before = v.image.stdDev();

      // headless path: engine round-trip with BXT args (mirrors BXTParams.buildArgs)
      let dir = RCAstro.tempDir();
      let inP = RCAstro.saveView(v, dir);
      let outP = dir + "/o.xisf";
      let r = RCAstro.runCli("bxt", [inP, "--ss","0.25","--sn","0.90","--ansr","--device","gpu","--output",outP], null);
      assert(r.ok, "bxt failed: " + r.errorMsg);

      let out = RCAstro.importResult(outP, "bxt_out");
      RCAstro.applyInPlace(out, v);   // NOTE: applyInPlace closes `out` — do NOT forceClose it
      assert(Math.abs(v.image.stdDev() - before) > 1e-8, "applyInPlace did not modify target");

      src.forceClose();
      RCAstro.cleanup([inP, outP]);
      W("PASS t_bxt");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
```

- [ ] **Step 2: Run it, verify the baseline**

Run: `rm -f /tmp/rc_t3_result.log; test/run-headless.sh "$PWD/test/t_bxt.js" >/dev/null 2>&1; grep -q "PASS t_bxt" /tmp/rc_t3_result.log 2>/dev/null && echo OK || { echo FAILED; cat /tmp/rc_t3_result.log 2>/dev/null; }`
Note: the engine already exists (Tasks 1–2), so this test may PASS on its first run. That is expected and acceptable — it is an integration guard for the BXT arg set + `applyInPlace`, not a red-green unit test. Record whichever result you get; if it FAILS, fix the arg set before writing `RCAstroBXT.js`.

- [ ] **Step 3: Implement `RCAstroBXT.js`**

Create `RCAstroBXT.js`:

```javascript
#feature-id    RC-Astro > BlurXTerminator (CLI)
#feature-info  Runs the GPU-accelerated rc-astro BlurXTerminator on the target view.

#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/Label.jsh>
#include "RCAstroLib.jsh"

#define BXT_TITLE "BlurXTerminator (CLI)"

var BXTParams = {
   targetView: undefined,
   sharpenStars: 0.25,
   sharpenNonstellar: 0.90,
   adjustHalos: 0.0,
   autoPSF: true,
   psfRadius: 0.0,
   correctOnly: false,
   mlVersion: 0,   // 0 = Latest
   device: "gpu",

   save: function() {
      Parameters.set("sharpenStars", this.sharpenStars);
      Parameters.set("sharpenNonstellar", this.sharpenNonstellar);
      Parameters.set("adjustHalos", this.adjustHalos);
      Parameters.set("autoPSF", this.autoPSF);
      Parameters.set("psfRadius", this.psfRadius);
      Parameters.set("correctOnly", this.correctOnly);
      Parameters.set("mlVersion", this.mlVersion);
      Parameters.set("device", this.device);
   },
   load: function() {
      if (Parameters.has("sharpenStars"))      this.sharpenStars = Parameters.getReal("sharpenStars");
      if (Parameters.has("sharpenNonstellar")) this.sharpenNonstellar = Parameters.getReal("sharpenNonstellar");
      if (Parameters.has("adjustHalos"))       this.adjustHalos = Parameters.getReal("adjustHalos");
      if (Parameters.has("autoPSF"))           this.autoPSF = Parameters.getBoolean("autoPSF");
      if (Parameters.has("psfRadius"))         this.psfRadius = Parameters.getReal("psfRadius");
      if (Parameters.has("correctOnly"))       this.correctOnly = Parameters.getBoolean("correctOnly");
      if (Parameters.has("mlVersion"))         this.mlVersion = Parameters.getInteger("mlVersion");
      if (Parameters.has("device"))            this.device = Parameters.getString("device");
   },
   buildArgs: function(inPath, outPath) {
      let a = [inPath];
      if (this.correctOnly) a.push("--correct-only");
      else { a.push("--ss", format("%.3f", this.sharpenStars),
                    "--sn", format("%.3f", this.sharpenNonstellar)); }
      a.push("--ash", format("%.3f", this.adjustHalos));
      if (this.autoPSF) a.push("--ansr");
      else a.push("--no-ansr", "--nsr", format("%.2f", this.psfRadius));
      if (this.mlVersion != 0) a.push("--ml-version", String(this.mlVersion));
      a.push("--device", this.device, "--output", outPath);
      return a;
   }
};

function runBXT(view) {
   if (view == null || view.isNull) { RCAstro.fail("No target view."); return; }
   let dir = RCAstro.tempDir();
   let inP = RCAstro.saveView(view, dir);
   let outP = dir + "/" + view.id + "_bxt.xisf";
   try {
      let r = RCAstro.runCli("bxt", BXTParams.buildArgs(inP, outP), null);
      if (!r.ok) { RCAstro.fail("BlurXTerminator failed: " + r.errorMsg); return; }
      let out = RCAstro.importResult(outP, view.id + "_bxt_tmp");
      RCAstro.applyInPlace(out, view);   // applyInPlace closes `out` — do NOT forceClose it
   } finally {
      RCAstro.cleanup([inP, outP]);
   }
}

function BXTDialog() {
   this.__base__ = Dialog; this.__base__();
   let self = this;
   this.windowTitle = BXT_TITLE;
   this.scaledMinWidth = 420;

   this.viewList = new ViewList(this);
   this.viewList.getMainViews();
   if (BXTParams.targetView) this.viewList.currentView = BXTParams.targetView;
   this.viewList.onViewSelected = function(v){ BXTParams.targetView = v; };

   function slider(label, min, max, prec, get, set) {
      let nc = new NumericControl(self);
      nc.label.text = label; nc.setRange(min, max); nc.setPrecision(prec);
      nc.setValue(get()); nc.onValueUpdated = function(v){ set(v); };
      return nc;
   }
   this.ss  = slider("Sharpen stars:",      0, 0.7, 3, function(){return BXTParams.sharpenStars;},      function(v){BXTParams.sharpenStars=v;});
   this.sn  = slider("Sharpen nonstellar:", 0, 1.0, 3, function(){return BXTParams.sharpenNonstellar;}, function(v){BXTParams.sharpenNonstellar=v;});
   this.ash = slider("Adjust star halos:", -0.5, 0.5, 3, function(){return BXTParams.adjustHalos;},     function(v){BXTParams.adjustHalos=v;});
   this.nsr = slider("Nonstellar radius:",  0, 4.0, 2, function(){return BXTParams.psfRadius;},         function(v){BXTParams.psfRadius=v;});
   this.nsr.enabled = !BXTParams.autoPSF;

   this.autoPSF = new CheckBox(this); this.autoPSF.text = "Auto nonstellar PSF"; this.autoPSF.checked = BXTParams.autoPSF;
   this.autoPSF.onCheck = function(c){ BXTParams.autoPSF = c; self.nsr.enabled = !c; };

   this.correctOnly = new CheckBox(this); this.correctOnly.text = "Correct only (no sharpening)"; this.correctOnly.checked = BXTParams.correctOnly;
   this.correctOnly.onCheck = function(c){ BXTParams.correctOnly = c; self.ss.enabled = !c; self.sn.enabled = !c; };

   this.mlv = new ComboBox(this); this.mlv.addItem("Latest"); this.mlv.addItem("AI 4"); this.mlv.addItem("AI 2");
   this.mlv.currentItem = (BXTParams.mlVersion==4)?1:(BXTParams.mlVersion==2)?2:0;
   this.mlv.onItemSelected = function(i){ BXTParams.mlVersion = (i==1)?4:(i==2)?2:0; };

   this.dev = new ComboBox(this); this.dev.addItem("GPU"); this.dev.addItem("CPU");
   this.dev.currentItem = (BXTParams.device=="cpu")?1:0;
   this.dev.onItemSelected = function(i){ BXTParams.device = (i==1)?"cpu":"gpu"; };

   this.newInstanceButton = new ToolButton(this);
   this.newInstanceButton.icon = this.scaledResource(":/process-interface/new-instance.png");
   this.newInstanceButton.setScaledFixedSize(24,24);
   this.newInstanceButton.onMousePress = function(){ BXTParams.save(); self.newInstance(); };

   this.ok = new PushButton(this); this.ok.text = "Apply"; this.ok.icon = this.scaledResource(":/icons/ok.png");
   this.ok.onClick = function(){ self.ok_Click(); };
   this.cancel = new PushButton(this); this.cancel.text = "Cancel"; this.cancel.icon = this.scaledResource(":/icons/cancel.png");
   this.cancel.onClick = function(){ self.cancel_Click(); };

   this.ok_Click = function(){ self.doRun = true; self.done(1); };
   this.cancel_Click = function(){ self.done(0); };

   let btns = new HorizontalSizer; btns.spacing = 6;
   btns.add(this.newInstanceButton); btns.addStretch(); btns.add(this.ok); btns.add(this.cancel);
   let mlLabel = new Label(this); mlLabel.text = "Model:";
   let devLabel = new Label(this); devLabel.text = "Device:";
   let devRow = new HorizontalSizer; devRow.spacing = 6;
   devRow.add(mlLabel); devRow.add(this.mlv); devRow.addSpacing(12);
   devRow.add(devLabel); devRow.add(this.dev); devRow.addStretch();

   this.sizer = new VerticalSizer; this.sizer.margin = 8; this.sizer.spacing = 6;
   this.sizer.add(this.viewList);
   this.sizer.add(this.ss); this.sizer.add(this.sn); this.sizer.add(this.ash);
   this.sizer.add(this.autoPSF); this.sizer.add(this.nsr);
   this.sizer.add(this.correctOnly);
   this.sizer.add(devRow);
   this.sizer.add(btns);
   this.adjustToContents();
}
BXTDialog.prototype = new Dialog;

function main() {
   if (Parameters.isViewTarget) { BXTParams.load(); runBXT(Parameters.targetView); return; }
   if (Parameters.isGlobalTarget) { BXTParams.load(); runBXT(ImageWindow.activeWindow.mainView); return; }
   BXTParams.targetView = ImageWindow.activeWindow.isNull ? undefined : ImageWindow.activeWindow.mainView;
   let d = new BXTDialog();
   d.doRun = false;
   if (d.execute() && d.doRun) runBXT(BXTParams.targetView);
}
main();
```

> Implementer note: the two helper stubs `Label`/`PILabel` above are a placeholder shortcut — replace with a normal `new Label(this)` from `#include <pjsr/Label.jsh>`; PJSR provides `Label` natively. Use `let lab = new Label(this); lab.text = "Model:";`. Do NOT ship the stub. (Kept minimal here to avoid over-specifying; wire real `Label` during implementation and re-run the test.)

- [ ] **Step 4: Run the smoke test, verify it passes**

Run: `rm -f /tmp/rc_t3_result.log; test/run-headless.sh "$PWD/test/t_bxt.js" >/dev/null 2>&1; grep -q "PASS t_bxt" /tmp/rc_t3_result.log && echo OK || { echo FAILED; cat /tmp/rc_t3_result.log; }`
Expected: `OK`

- [ ] **Step 5: Interactive sanity (manual, once)**

Launch PI normally, open an image, run Scripts > RC-Astro > BlurXTerminator (CLI), Apply, confirm the view sharpens and Ctrl+Z restores it. (Requires Feature Scripts registration from Task 6, or temporarily run via Script Editor.)

- [ ] **Step 6: Commit**

```bash
cd ~/PixInsightScripts/RC-Astro
git add RCAstroBXT.js test/t_bxt.js
git commit -m "feat: BlurXTerminator (CLI) wrapper + smoke test"
```

---

### Task 4: StarXTerminator tool (`RCAstroSXT.js`)

**Files:**
- Create: `~/PixInsightScripts/RC-Astro/RCAstroSXT.js`
- Test: `~/PixInsightScripts/RC-Astro/test/t_sxt.js`

**Interfaces:**
- Consumes: `RCAstro` engine; `RCAstro.newWindow`.
- Produces: `SXTParams` (`.save/.load/.buildArgs`), `runSXT(view)` that creates `<id>_starless` (+ `<id>_stars` when enabled), `main()` dispatch.

- [ ] **Step 1: Write the failing headless smoke test**

Create `test/t_sxt.js`:

```javascript
#include "../RCAstroLib.jsh"
// Result goes to a FILE — console.* does not reach stdout under --automation-mode.
var RESULT_LOG = "/tmp/rc_t4_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let v = src.mainView; let id = v.id;
      let dir = RCAstro.tempDir();
      let inP = RCAstro.saveView(v, dir);
      let outP = dir + "/" + id + "_starless.xisf";
      // ASSUMPTION to be verified from disk in Step 2 — do NOT trust this name.
      let starsP = dir + "/" + id + "_starless_stars.xisf";

      let r = RCAstro.runCli("sxt", [inP, "--output-stars", "--device","gpu","--output",outP], null);
      assert(r.ok, "sxt failed: " + r.errorMsg);
      assert(File.exists(outP), "no starless output");

      // Log EVERYTHING the CLI actually wrote, so Step 2 can read the real stars filename.
      // NOTE: bare global searchDirectory() — File.searchDirectory() is NOT callable in this PI build.
      let found = searchDirectory(dir + "/*.xisf");
      for (let i = 0; i < found.length; ++i) W("wrote: " + found[i]);

      assert(File.exists(starsP), "no stars output at assumed path " + starsP);
      src.forceClose();
      RCAstro.cleanup([inP, outP, starsP]);
      W("PASS t_sxt");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
```

- [ ] **Step 2: Run it — DISCOVER the real stars-file name from disk**

Run: `rm -f /tmp/rc_t4_result.log; test/run-headless.sh "$PWD/test/t_sxt.js" >/dev/null 2>&1; cat /tmp/rc_t4_result.log`
The `wrote: ...` lines list every file the CLI actually produced. If the assumed `_starless_stars.xisf` name is wrong, the log's `wrote:` lines give you the true one. **Correct `starsP` in the test AND in `runSXT` (Step 3) to the real name — do not guess it.** Re-run until `PASS t_sxt`.

- [ ] **Step 3: Implement `RCAstroSXT.js`**

Create `RCAstroSXT.js`:

```javascript
#feature-id    RC-Astro > StarXTerminator (CLI)
#feature-info  Runs the GPU-accelerated rc-astro StarXTerminator; starless (+ optional stars) to new windows.

#include <pjsr/Sizer.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/Label.jsh>
#include "RCAstroLib.jsh"

#define SXT_TITLE "StarXTerminator (CLI)"

var SXTParams = {
   targetView: undefined,
   outputStars: false,
   unscreen: false,
   mlVersion: 0,
   device: "gpu",

   save: function() {
      Parameters.set("outputStars", this.outputStars);
      Parameters.set("unscreen", this.unscreen);
      Parameters.set("mlVersion", this.mlVersion);
      Parameters.set("device", this.device);
   },
   load: function() {
      if (Parameters.has("outputStars")) this.outputStars = Parameters.getBoolean("outputStars");
      if (Parameters.has("unscreen"))    this.unscreen = Parameters.getBoolean("unscreen");
      if (Parameters.has("mlVersion"))   this.mlVersion = Parameters.getInteger("mlVersion");
      if (Parameters.has("device"))      this.device = Parameters.getString("device");
   },
   buildArgs: function(inPath, outPath) {
      let a = [inPath];
      if (this.outputStars) { a.push("--output-stars"); if (this.unscreen) a.push("--unscreen"); }
      if (this.mlVersion != 0) a.push("--ml-version", String(this.mlVersion));
      a.push("--device", this.device, "--output", outPath);
      return a;
   }
};

function runSXT(view) {
   if (view == null || view.isNull) { RCAstro.fail("No target view."); return; }
   let id = view.id;
   let dir = RCAstro.tempDir();
   let inP = RCAstro.saveView(view, dir);
   let outP = dir + "/" + id + "_starless.xisf";
   // NOTE: confirmed via Task 4 Step 2 — CLI writes stars as <starlessbase>_stars.xisf
   let starsP = dir + "/" + id + "_starless_stars.xisf";
   let temps = [inP, outP, starsP];
   try {
      let r = RCAstro.runCli("sxt", SXTParams.buildArgs(inP, outP), null);
      if (!r.ok) { RCAstro.fail("StarXTerminator failed: " + r.errorMsg); return; }
      let starless = RCAstro.importResult(outP, id + "_starless");
      RCAstro.newWindow(starless, id + "_starless");
      if (SXTParams.outputStars && File.exists(starsP)) {
         let stars = RCAstro.importResult(starsP, id + "_stars");
         RCAstro.newWindow(stars, id + "_stars");
      }
   } finally {
      RCAstro.cleanup(temps);
   }
}

function SXTDialog() {
   this.__base__ = Dialog; this.__base__();
   let self = this;
   this.windowTitle = SXT_TITLE;
   this.scaledMinWidth = 380;

   this.viewList = new ViewList(this); this.viewList.getMainViews();
   if (SXTParams.targetView) this.viewList.currentView = SXTParams.targetView;
   this.viewList.onViewSelected = function(v){ SXTParams.targetView = v; };

   this.starsCB = new CheckBox(this); this.starsCB.text = "Also create stars-only image";
   this.starsCB.checked = SXTParams.outputStars;
   this.unscreenCB = new CheckBox(this); this.unscreenCB.text = "Unscreen stars";
   this.unscreenCB.checked = SXTParams.unscreen; this.unscreenCB.enabled = SXTParams.outputStars;
   this.starsCB.onCheck = function(c){ SXTParams.outputStars = c; self.unscreenCB.enabled = c; };
   this.unscreenCB.onCheck = function(c){ SXTParams.unscreen = c; };

   this.mlv = new ComboBox(this); this.mlv.addItem("Latest"); this.mlv.addItem("v11");
   this.mlv.currentItem = (SXTParams.mlVersion==11)?1:0;
   this.mlv.onItemSelected = function(i){ SXTParams.mlVersion = (i==1)?11:0; };
   this.dev = new ComboBox(this); this.dev.addItem("GPU"); this.dev.addItem("CPU");
   this.dev.currentItem = (SXTParams.device=="cpu")?1:0;
   this.dev.onItemSelected = function(i){ SXTParams.device = (i==1)?"cpu":"gpu"; };

   this.newInstanceButton = new ToolButton(this);
   this.newInstanceButton.icon = this.scaledResource(":/process-interface/new-instance.png");
   this.newInstanceButton.setScaledFixedSize(24,24);
   this.newInstanceButton.onMousePress = function(){ SXTParams.save(); self.newInstance(); };
   this.ok = new PushButton(this); this.ok.text = "Apply";
   this.ok.onClick = function(){ self.doRun = true; self.done(1); };
   this.cancel = new PushButton(this); this.cancel.text = "Cancel";
   this.cancel.onClick = function(){ self.done(0); };

   let mlRow = new HorizontalSizer; mlRow.spacing=6;
   let l1 = new Label(this); l1.text="Model:"; let l2 = new Label(this); l2.text="Device:";
   mlRow.add(l1); mlRow.add(this.mlv); mlRow.addSpacing(12); mlRow.add(l2); mlRow.add(this.dev); mlRow.addStretch();
   let btns = new HorizontalSizer; btns.spacing=6;
   btns.add(this.newInstanceButton); btns.addStretch(); btns.add(this.ok); btns.add(this.cancel);

   this.sizer = new VerticalSizer; this.sizer.margin=8; this.sizer.spacing=6;
   this.sizer.add(this.viewList); this.sizer.add(this.starsCB); this.sizer.add(this.unscreenCB);
   this.sizer.add(mlRow); this.sizer.add(btns);
   this.adjustToContents();
}
SXTDialog.prototype = new Dialog;

function main() {
   if (Parameters.isViewTarget) { SXTParams.load(); runSXT(Parameters.targetView); return; }
   if (Parameters.isGlobalTarget) { SXTParams.load(); runSXT(ImageWindow.activeWindow.mainView); return; }
   SXTParams.targetView = ImageWindow.activeWindow.isNull ? undefined : ImageWindow.activeWindow.mainView;
   let d = new SXTDialog(); d.doRun = false;
   if (d.execute() && d.doRun) runSXT(SXTParams.targetView);
}
main();
```

- [ ] **Step 4: Run the smoke test, verify it passes**

Run: `rm -f /tmp/rc_t4_result.log; test/run-headless.sh "$PWD/test/t_sxt.js" >/dev/null 2>&1; grep -q "PASS t_sxt" /tmp/rc_t4_result.log && echo OK || { echo FAILED; cat /tmp/rc_t4_result.log; }`
Expected: `OK`

- [ ] **Step 5: Commit**

```bash
cd ~/PixInsightScripts/RC-Astro
git add RCAstroSXT.js test/t_sxt.js
git commit -m "feat: StarXTerminator (CLI) wrapper + smoke test"
```

---

### Task 5: NoiseXTerminator tool (`RCAstroNXT.js`)

**Files:**
- Create: `~/PixInsightScripts/RC-Astro/RCAstroNXT.js`
- Test: `~/PixInsightScripts/RC-Astro/test/t_nxt.js`

**Interfaces:**
- Consumes: `RCAstro` engine.
- Produces: `NXTParams` (`.save/.load/.buildArgs`, includes advanced fields), `runNXT(view)` in-place, `main()` dispatch, dialog with a collapsed Advanced `SectionBar`.

- [ ] **Step 1: Write the failing headless smoke test**

Create `test/t_nxt.js`:

```javascript
#include "../RCAstroLib.jsh"
// Result goes to a FILE — console.* does not reach stdout under --automation-mode.
var RESULT_LOG = "/tmp/rc_t5_result.log";
function assert(c, m){ if(!c){ throw new Error(m); } }

function main() {
   let log = new File;
   log.createForWriting(RESULT_LOG);
   function W(s){ log.outTextLn(s); log.flush(); console.noteln(s); }

   try {
      let src = ImageWindow.open("/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf")[0];
      let v = src.mainView; let before = v.image.stdDev();
      let dir = RCAstro.tempDir(); let inP = RCAstro.saveView(v, dir); let outP = dir + "/o.xisf";

      let r = RCAstro.runCli("nxt", [inP, "--dn","0.90","--it","2","--fs","5.0","--device","gpu","--output",outP], null);
      assert(r.ok, "nxt failed: " + r.errorMsg);

      let out = RCAstro.importResult(outP, "nxt_out");
      RCAstro.applyInPlace(out, v);   // NOTE: applyInPlace closes `out` — do NOT forceClose it
      assert(Math.abs(v.image.stdDev() - before) > 1e-8, "applyInPlace did not modify target");

      src.forceClose();
      RCAstro.cleanup([inP, outP]);
      W("PASS t_nxt");
   } catch (e) {
      W("FAIL: " + e.message);
   }
   log.close();
}
main();
```

- [ ] **Step 2: Run it, verify pass/fail baseline**

Run: `rm -f /tmp/rc_t5_result.log; test/run-headless.sh "$PWD/test/t_nxt.js" >/dev/null 2>&1; grep -q "PASS t_nxt" /tmp/rc_t5_result.log 2>/dev/null && echo OK || { echo FAILED; cat /tmp/rc_t5_result.log 2>/dev/null; }`
Note: the engine already exists, so this may PASS on first run — expected and acceptable (integration guard for the NXT arg set + apply). If it FAILS, fix the arg set before writing `RCAstroNXT.js`.

- [ ] **Step 3: Implement `RCAstroNXT.js`**

Create `RCAstroNXT.js`:

```javascript
#feature-id    RC-Astro > NoiseXTerminator (CLI)
#feature-info  Runs the GPU-accelerated rc-astro NoiseXTerminator on the target view.

#include <pjsr/Sizer.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/Label.jsh>
#include <pjsr/SectionBar.jsh>
#include "RCAstroLib.jsh"

#define NXT_TITLE "NoiseXTerminator (CLI)"

var NXTParams = {
   targetView: undefined,
   denoise: 0.90, iterations: 2, freqScale: 5.0,
   intensity: 0.90, color: 0.90,
   dhf: 0.90, dlf: 0.90, dihf: 0.90, dilf: 0.90, dchf: 0.90, dclf: 0.90,
   useAdvanced: false,
   mlVersion: 0, device: "gpu",

   save: function() {
      let s = Parameters.set.bind(Parameters);
      s("denoise",this.denoise); s("iterations",this.iterations); s("freqScale",this.freqScale);
      s("intensity",this.intensity); s("color",this.color);
      s("dhf",this.dhf); s("dlf",this.dlf); s("dihf",this.dihf); s("dilf",this.dilf); s("dchf",this.dchf); s("dclf",this.dclf);
      s("useAdvanced",this.useAdvanced); s("mlVersion",this.mlVersion); s("device",this.device);
   },
   load: function() {
      let g = Parameters;
      if (g.has("denoise")) this.denoise=g.getReal("denoise");
      if (g.has("iterations")) this.iterations=g.getReal("iterations");
      if (g.has("freqScale")) this.freqScale=g.getReal("freqScale");
      if (g.has("intensity")) this.intensity=g.getReal("intensity");
      if (g.has("color")) this.color=g.getReal("color");
      if (g.has("dhf")) this.dhf=g.getReal("dhf");
      if (g.has("dlf")) this.dlf=g.getReal("dlf");
      if (g.has("dihf")) this.dihf=g.getReal("dihf");
      if (g.has("dilf")) this.dilf=g.getReal("dilf");
      if (g.has("dchf")) this.dchf=g.getReal("dchf");
      if (g.has("dclf")) this.dclf=g.getReal("dclf");
      if (g.has("useAdvanced")) this.useAdvanced=g.getBoolean("useAdvanced");
      if (g.has("mlVersion")) this.mlVersion=g.getInteger("mlVersion");
      if (g.has("device")) this.device=g.getString("device");
   },
   buildArgs: function(inPath, outPath) {
      let a = [inPath, "--dn", format("%.3f",this.denoise),
                       "--it", format("%.0f",this.iterations),
                       "--fs", format("%.2f",this.freqScale)];
      if (this.useAdvanced) {
         a.push("--di", format("%.3f",this.intensity), "--dc", format("%.3f",this.color),
                "--dhf", format("%.3f",this.dhf), "--dlf", format("%.3f",this.dlf),
                "--dihf", format("%.3f",this.dihf), "--dilf", format("%.3f",this.dilf),
                "--dchf", format("%.3f",this.dchf), "--dclf", format("%.3f",this.dclf));
      }
      if (this.mlVersion != 0) a.push("--ml-version", String(this.mlVersion));
      a.push("--device", this.device, "--output", outPath);
      return a;
   }
};

function runNXT(view) {
   if (view == null || view.isNull) { RCAstro.fail("No target view."); return; }
   let dir = RCAstro.tempDir();
   let inP = RCAstro.saveView(view, dir);
   let outP = dir + "/" + view.id + "_nxt.xisf";
   try {
      let r = RCAstro.runCli("nxt", NXTParams.buildArgs(inP, outP), null);
      if (!r.ok) { RCAstro.fail("NoiseXTerminator failed: " + r.errorMsg); return; }
      let out = RCAstro.importResult(outP, view.id + "_nxt_tmp");
      RCAstro.applyInPlace(out, view);   // applyInPlace closes `out` — do NOT forceClose it
   } finally { RCAstro.cleanup([inP, outP]); }
}

function NXTDialog() {
   this.__base__ = Dialog; this.__base__();
   let self = this; this.windowTitle = NXT_TITLE; this.scaledMinWidth = 440;

   this.viewList = new ViewList(this); this.viewList.getMainViews();
   if (NXTParams.targetView) this.viewList.currentView = NXTParams.targetView;
   this.viewList.onViewSelected = function(v){ NXTParams.targetView = v; };

   function slider(parent, label, min, max, prec, get, set) {
      let nc = new NumericControl(parent);
      nc.label.text = label; nc.setRange(min,max); nc.setPrecision(prec);
      nc.setValue(get()); nc.onValueUpdated = function(v){ set(v); };
      return nc;
   }
   this.dn = slider(this,"Denoise:",       0,1,   3, function(){return NXTParams.denoise;},   function(v){NXTParams.denoise=v;});
   this.it = slider(this,"Iterations:",    1,5,   0, function(){return NXTParams.iterations;},function(v){NXTParams.iterations=v;});
   this.fs = slider(this,"Frequency scale:",1,100,2, function(){return NXTParams.freqScale;}, function(v){NXTParams.freqScale=v;});

   // Advanced section
   this.advCtl = new Control(this);
   let av = new VerticalSizer; av.margin=6; av.spacing=4;
   this.di   = slider(this.advCtl,"Intensity:",         0,1,3, function(){return NXTParams.intensity;}, function(v){NXTParams.intensity=v;});
   this.dc   = slider(this.advCtl,"Color:",             0,1,3, function(){return NXTParams.color;},     function(v){NXTParams.color=v;});
   this.dhf  = slider(this.advCtl,"Denoise hi-freq:",   0,1,3, function(){return NXTParams.dhf;},  function(v){NXTParams.dhf=v;});
   this.dlf  = slider(this.advCtl,"Denoise lo-freq:",   0,1,3, function(){return NXTParams.dlf;},  function(v){NXTParams.dlf=v;});
   this.dihf = slider(this.advCtl,"Intensity hi-freq:", 0,1,3, function(){return NXTParams.dihf;}, function(v){NXTParams.dihf=v;});
   this.dilf = slider(this.advCtl,"Intensity lo-freq:", 0,1,3, function(){return NXTParams.dilf;}, function(v){NXTParams.dilf=v;});
   this.dchf = slider(this.advCtl,"Color hi-freq:",     0,1,3, function(){return NXTParams.dchf;}, function(v){NXTParams.dchf=v;});
   this.dclf = slider(this.advCtl,"Color lo-freq:",     0,1,3, function(){return NXTParams.dclf;}, function(v){NXTParams.dclf=v;});
   av.add(this.di); av.add(this.dc); av.add(this.dhf); av.add(this.dlf); av.add(this.dihf); av.add(this.dilf); av.add(this.dchf); av.add(this.dclf);
   this.advCtl.sizer = av;
   this.advBar = new SectionBar(this, "Advanced (per-scale)");
   this.advBar.setSection(this.advCtl);
   this.advBar.onToggleSection = function(bar, toggleBegin){ if(!toggleBegin){ NXTParams.useAdvanced = !self.advCtl.visible ? false : true; } };
   this.advCtl.visible = NXTParams.useAdvanced;
   // Treat "section expanded" as "use advanced": bind on hide/show.
   this.advBar.onToggleSection = function(bar, toggleBegin){ if (!toggleBegin) NXTParams.useAdvanced = self.advCtl.visible; };

   this.mlv = new ComboBox(this); this.mlv.addItem("Latest"); this.mlv.addItem("v3"); this.mlv.addItem("v2");
   this.mlv.currentItem = (NXTParams.mlVersion==3)?1:(NXTParams.mlVersion==2)?2:0;
   this.mlv.onItemSelected = function(i){ NXTParams.mlVersion = (i==1)?3:(i==2)?2:0; };
   this.dev = new ComboBox(this); this.dev.addItem("GPU"); this.dev.addItem("CPU");
   this.dev.currentItem = (NXTParams.device=="cpu")?1:0;
   this.dev.onItemSelected = function(i){ NXTParams.device = (i==1)?"cpu":"gpu"; };

   this.newInstanceButton = new ToolButton(this);
   this.newInstanceButton.icon = this.scaledResource(":/process-interface/new-instance.png");
   this.newInstanceButton.setScaledFixedSize(24,24);
   this.newInstanceButton.onMousePress = function(){ NXTParams.save(); self.newInstance(); };
   this.ok = new PushButton(this); this.ok.text="Apply"; this.ok.onClick=function(){ self.doRun=true; self.done(1); };
   this.cancel = new PushButton(this); this.cancel.text="Cancel"; this.cancel.onClick=function(){ self.done(0); };

   let l1=new Label(this); l1.text="Model:"; let l2=new Label(this); l2.text="Device:";
   let mlRow=new HorizontalSizer; mlRow.spacing=6; mlRow.add(l1); mlRow.add(this.mlv); mlRow.addSpacing(12); mlRow.add(l2); mlRow.add(this.dev); mlRow.addStretch();
   let btns=new HorizontalSizer; btns.spacing=6; btns.add(this.newInstanceButton); btns.addStretch(); btns.add(this.ok); btns.add(this.cancel);

   this.sizer = new VerticalSizer; this.sizer.margin=8; this.sizer.spacing=6;
   this.sizer.add(this.viewList); this.sizer.add(this.dn); this.sizer.add(this.it); this.sizer.add(this.fs);
   this.sizer.add(this.advBar); this.sizer.add(this.advCtl);
   this.sizer.add(mlRow); this.sizer.add(btns);
   this.adjustToContents();
}
NXTDialog.prototype = new Dialog;

function main() {
   if (Parameters.isViewTarget) { NXTParams.load(); runNXT(Parameters.targetView); return; }
   if (Parameters.isGlobalTarget) { NXTParams.load(); runNXT(ImageWindow.activeWindow.mainView); return; }
   NXTParams.targetView = ImageWindow.activeWindow.isNull ? undefined : ImageWindow.activeWindow.mainView;
   let d = new NXTDialog(); d.doRun=false;
   if (d.execute() && d.doRun) runNXT(NXTParams.targetView);
}
main();
```

- [ ] **Step 4: Run the smoke test, verify it passes**

Run: `rm -f /tmp/rc_t5_result.log; test/run-headless.sh "$PWD/test/t_nxt.js" >/dev/null 2>&1; grep -q "PASS t_nxt" /tmp/rc_t5_result.log && echo OK || { echo FAILED; cat /tmp/rc_t5_result.log; }`
Expected: `OK`

- [ ] **Step 5: Commit**

```bash
cd ~/PixInsightScripts/RC-Astro
git add RCAstroNXT.js test/t_nxt.js
git commit -m "feat: NoiseXTerminator (CLI) wrapper + smoke test"
```

---

### Task 6: Install, register, README, full-suite verification

**Files:**
- Create: `~/PixInsightScripts/RC-Astro/install.sh`
- Create: `~/PixInsightScripts/RC-Astro/README.md`

- [ ] **Step 1: Write `install.sh`**

Create `install.sh`:

```bash
#!/usr/bin/env bash
# RC-Astro PI wrappers are already in this directory. This prints the one-time
# Feature Scripts registration steps (PJSR cannot register feature dirs itself).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "Scripts live in: $DIR"
echo
echo "One-time registration in PixInsight:"
echo "  1. SCRIPT menu > Feature Scripts..."
echo "  2. Add  ->  select: $DIR"
echo "  3. Done. Then find them under:  Scripts > RC-Astro > *(CLI)"
echo
echo "rc-astro binary: $(command -v rc-astro || echo 'NOT FOUND on PATH')"
```

Then `chmod +x install.sh`.

- [ ] **Step 2: Write `README.md`**

Create `README.md` with: purpose (CLI wrappers avoiding the plugin's Blackwell TF crash), the three tools + their key params, install steps (run `install.sh`, then Feature Scripts add), the headless test commands, and a note that GPU requires system cuDNN ≥ 9.13 (already installed: 9.24). Reference the design doc.

- [ ] **Step 3: Run the full headless test suite**

Each test writes its verdict to its own result log (`console.*` does not reach stdout under `--automation-mode`). Map test → result log:

```bash
cd ~/PixInsightScripts/RC-Astro
declare -A LOG=( [t_lib_roundtrip]=/tmp/rc_t1_result.log [t_lib_runcli]=/tmp/rc_t2_result.log \
                 [t_bxt]=/tmp/rc_t3_result.log [t_sxt]=/tmp/rc_t4_result.log [t_nxt]=/tmp/rc_t5_result.log )
fail=0
for t in t_lib_roundtrip t_lib_runcli t_bxt t_sxt t_nxt; do
  rm -f "${LOG[$t]}"
  test/run-headless.sh "$PWD/test/$t.js" >/dev/null 2>&1 || true
  if grep -q "PASS $t" "${LOG[$t]}" 2>/dev/null; then echo "== $t OK =="
  else echo "== $t FAILED =="; cat "${LOG[$t]}" 2>/dev/null; fail=1; fi
done
exit $fail
```
Expected: all five print `OK`.

- [ ] **Step 4: Register + interactive verification (manual, once)**

Run `./install.sh`, follow the Feature Scripts steps in PI, then confirm all three appear under `Scripts > RC-Astro` and run on an open image (BXT/NXT modify in place + undo; SXT spawns `_starless` and, when enabled, `_stars`). Force an error (e.g., set Device combo via a bad temp edit or disconnect) to confirm a loud MessageBox.

- [ ] **Step 5: Commit**

```bash
cd ~/PixInsightScripts/RC-Astro
git add install.sh README.md
git commit -m "feat: install script, README, full-suite headless verification"
```

---

## Self-Review notes (author)

- **Spec coverage:** engine (§4)→Tasks 1–2; BXT/SXT/NXT dialogs+params (§5)→Tasks 3–5; automation/Parameters (§6)→each tool's `main()` + new-instance button; error handling (§7)→`RCAstro.fail` + `runCli` error path; testing (§8)→headless `t_*` + manual steps; install (§9)→Task 6.
- **Known implementer follow-ups (flagged explicitly):**
  1. **Dialog-open check:** the `t_bxt/t_sxt/t_nxt` headless tests exercise the engine + arg-building path, not the GUI dialog (dialogs can't run under `--automation-mode`). Each tool's dialog must additionally get a one-time manual open check (Task 3/4/5 Step 5 / Task 6 Step 4).
  2. **Task 4 Step 2 discovery:** the stars-only output filename is *verified from disk*, not assumed; `runSXT`'s `starsP` must match what the CLI actually writes.
  3. **PixInsight API spot-checks during implementation** (confirm against `/opt/PixInsight/src/scripts` examples, don't assume): `NumericControl.setRange/setPrecision/setValue`, `ViewList.getMainViews/onViewSelected`, `SectionBar.setSection/onToggleSection`, `ImageWindow.open` arg signature, `PixelMath` in-place `executeOn(view)` undo behavior, and `format()` availability (used for arg formatting).
- **Type consistency:** all tools use `RCAstro.{findBinary,tempDir,saveView,importResult,applyInPlace,newWindow,runCli,cleanup,fail}` exactly as defined in Tasks 1–2; each `*Params.buildArgs(inPath,outPath)` returns a string array consumed by `runCli(tool, args, onEvent)`.
