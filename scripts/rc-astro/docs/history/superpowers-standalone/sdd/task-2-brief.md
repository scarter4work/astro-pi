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

