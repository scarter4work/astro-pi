// RC-Astro CLI engine — shared by RCAstro{BXT,SXT,NXT}.js
#include <pjsr/UndoFlag.jsh>
#include <pjsr/StdIcon.jsh>
#include <pjsr/StdButton.jsh>

var RCAstro = {
   TITLE: "RC-Astro CLI",
   binaryPath: null,

   // When false (the default — correct for headless/automation use), fail()
   // never pops a modal dialog: it logs via console.criticalln() and throws,
   // which is safe under `--automation-mode` (no one is present to dismiss a
   // MessageBox, so a modal there is an indefinite hang). Interactive callers
   // (a tool's dialog-driven main()) should set RCAstro.interactive = true
   // before calling into the engine so failures are also shown to the user.
   interactive: false,

   findBinary: function() {
      if (this.binaryPath && File.exists(this.binaryPath)) return this.binaryPath;
      let candidates = ["/usr/local/bin/rc-astro"];
      // resolve via PATH -- best-effort only. The File.exists candidate loop
      // below is authoritative, so if `which` itself is missing (p.start()
      // throwing) we simply fall through with no extra candidate rather than
      // letting a raw exception escape findBinary().
      let which = "";
      try {
         let p = new ExternalProcess;
         p.start("/usr/bin/which rc-astro");
         // NOTE: CoreApplication.processEvents() is documented but is not a
         // callable function in this PixInsight 1.9.4 "Lockhart" build
         // (verified: typeof CoreApplication.processEvents === "undefined").
         // The bare global processEvents() (Global.processEvents, deprecated
         // in the docs but functional here) is used instead.
         for (; p.isStarting;) processEvents();
         for (; p.isRunning;)  processEvents();
         which = String(p.stdout).trim();
      } catch (e) {
         console.warningln("RC-Astro: `which rc-astro` could not be run (" + e.message + "); " +
                            "falling back to the fixed candidate path list.");
      }
      if (which.length > 0) candidates.push(which);
      for (let i = 0; i < candidates.length; ++i)
         if (candidates[i] && File.exists(candidates[i])) { this.binaryPath = candidates[i]; return candidates[i]; }
      return null;
   },

   // Directories created here are recorded so cleanup() can remove them later.
   _tempDirs: [],

   tempDir: function() {
      let base = (typeof(getEnvironmentVariable) != 'undefined' && getEnvironmentVariable("TMPDIR"))
                 ? getEnvironmentVariable("TMPDIR") : File.systemTempDirectory;
      let dir = base + "/rc-astro-" + Math.trunc(Date.now()) + "-" + Math.trunc(Math.random()*1e6);
      if (!File.directoryExists(dir)) File.createDirectory(dir, true);
      this._tempDirs.push(dir);
      return dir;
   },

   saveView: function(view, dir) {
      // View has no `mainView` property (verified against the PJSR docs:
      // /opt/PixInsight/doc/pjsr/objects/View/View.html lists isMainView,
      // isPreview, window, etc. -- no mainView). Dropping this on a preview
      // used to hit `undefined.window` and throw a raw, unhelpful TypeError.
      // Fail loudly and specifically instead; preview support is out of scope.
      if (!view.isMainView)
         this.fail("RC-Astro: only main views are supported (previews are not). Target: " + view.id);
      let win = view.window;
      let path = dir + "/" + view.id + ".xisf";

      // ImageWindow.saveAs() has Save-As semantics: it re-binds THIS window's
      // filePath to the new path (verified empirically -- see
      // test/probe_fix5_saveas.js / fix-wave-b-report.md). Since `win` here is
      // the user's real, on-screen window, saving straight to a throwaway temp
      // path would leave the user's window pointing at a file that
      // RCAstro.cleanup() later deletes -- their next Ctrl+S would silently
      // stop overwriting the original image file.
      //
      // Fix: never call saveAs() on the source window itself. Build an
      // independent, off-screen ImageWindow with matching geometry/sample
      // format, copy the pixel data into it with Image.assign() (the
      // canonical PJSR pattern used by bundled scripts, e.g.
      // AstroMarkSignatureAdder.js, Halo-B-Gon.js), save AND close *that*
      // temporary window, and leave the caller's real window/view completely
      // untouched. Also verified empirically (test/probe_fix5_newwin.js) that
      // this leaves the original window's filePath and pixel data intact.
      //
      // NOTE: `new ImageWindow(existingWindow)` is NOT a safe alternative --
      // verified empirically (test/probe_fix5_dup.js) that in this PJSR build
      // it does not create an independent copy: it aliases the SAME
      // underlying window, so saveAs()/forceClose() on the "duplicate"
      // mutated and then destroyed the original.
      let srcImg = win.mainView.image;
      let tmpWin = new ImageWindow(srcImg.width, srcImg.height, srcImg.numberOfChannels,
                                    srcImg.bitsPerSample, srcImg.isReal, srcImg.isColor);
      let ok = false;
      try {
         tmpWin.mainView.beginProcess();
         tmpWin.mainView.image.assign(srcImg);
         tmpWin.mainView.endProcess();
         // Carry FITS keywords along so the temp file handed to the rc-astro
         // CLI (and anything it echoes back) isn't silently stripped of
         // metadata relative to the pre-fix behavior of saving the real window.
         tmpWin.keywords = win.keywords;
         // saveAs(path, queryOptions=false, allowMessages=false, strict=true, verifyOverwrite=false)
         ok = tmpWin.saveAs(path, false, false, true, false);
      } finally {
         tmpWin.forceClose();
      }
      if (!ok)
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
   //
   // OWNERSHIP: applyInPlace takes ownership of resultWindow and closes it
   // (forceClose()) once its pixels have been copied into targetView.
   // Callers must NOT forceClose resultWindow themselves afterward.
   applyInPlace: function(resultWindow, targetView) {
      let P = new PixelMath;
      P.expression = resultWindow.mainView.id;
      P.useSingleExpression = true;
      P.generateOutput = true;
      P.createNewImage = false;
      P.rescale = false;
      P.truncate = false;
      // NOTE: PixelMath.SameAsTarget is undefined in this PJSR build (verified
      // empirically: typeof PixelMath.SameAsTarget === "undefined", causes
      // "invalid argument type: unsigned integer value expected."). The
      // correct constant lives on the prototype, matching every bundled PI
      // reference script (AstroMarkSignatureAdder.js, MaskMerge.js,
      // DonutRepair.js, BlemishBlaster.js, AdvStarmask.js, ...).
      P.newImageColorSpace = PixelMath.prototype.SameAsTarget;
      P.newImageSampleFormat = PixelMath.prototype.SameAsTarget;
      // executeOn() returns Boolean and MUST be checked. PixelMath silently
      // declines (locked view, geometry/colorspace mismatch, ...) rather than
      // throwing, so ignoring the return value here used to mean: a failed
      // apply looked identical to a successful one from every caller's point
      // of view -- targetView is left completely unmodified, but runBXT/
      // runNXT/etc. return normally as if the rc-astro result had been
      // applied. In a headless batch (e.g. a 12-panel mosaic), that is
      // exactly the silent-fallback failure mode this project forbids.
      //
      // Ownership contract (unchanged, still documented above): applyInPlace
      // owns resultWindow and must close it on EVERY path -- success or
      // failure -- so callers' `finally` blocks (which only clean up temp
      // FILES) never strand the hidden result ImageWindow (a full-resolution
      // float image) in memory/swap after a failed apply.
      let ok = P.executeOn(targetView);
      resultWindow.forceClose();
      if (!ok)
         this.fail("PixelMath failed to apply the rc-astro result to " + targetView.id);
   },

   newWindow: function(resultWindow, id) {
      resultWindow.mainView.id = id;
      resultWindow.show();
      resultWindow.zoomToFit();
      return resultWindow;
   },

   // Removes the given files, then removes any per-run temp directories
   // created by tempDir() so far. Signature is intentionally unchanged
   // (cleanup(paths)) — later tasks call this as RCAstro.cleanup([inP, outP]).
   //
   // This must never throw (it runs from `finally` blocks), but it must
   // never fail silently either: any file or directory that cannot be
   // removed produces a visible console.warningln(), never a swallowed
   // exception. File.removeDirectory() throws if the directory is not
   // empty, so before calling it we sweep up any files a caller forgot
   // to pass in explicitly.
   cleanup: function(paths) {
      for (let i = 0; i < paths.length; ++i) {
         try {
            if (paths[i] && File.exists(paths[i])) File.remove(paths[i]);
         } catch (e) {
            console.warningln("RC-Astro: could not remove temp file " + paths[i] + ": " + e.message);
         }
      }
      for (let i = 0; i < this._tempDirs.length; ++i) {
         let dir = this._tempDirs[i];
         try {
            if (!File.directoryExists(dir)) continue;
            // Sweep up any leftover files so a caller forgetting to pass
            // one into cleanup(paths) doesn't strand the whole directory.
            // NOTE: File.searchDirectory() is documented but is NOT a
            // callable function in this PixInsight 1.9.4 "Lockhart" build
            // (verified: typeof File.searchDirectory === "undefined", same
            // gotcha as CoreApplication.processEvents above). The bare
            // global searchDirectory() works and returns full paths
            // already (verified empirically) — do not re-prepend dir.
            let leftovers = searchDirectory(dir + "/*");
            for (let j = 0; j < leftovers.length; ++j) {
               let leftover = leftovers[j];
               try {
                  if (File.directoryExists(leftover)) File.removeDirectory(leftover);
                  else if (File.exists(leftover)) File.remove(leftover);
               } catch (e) {
                  console.warningln("RC-Astro: could not remove leftover temp item " + leftover + ": " + e.message);
               }
            }
            File.removeDirectory(dir);
         } catch (e) {
            console.warningln("RC-Astro: could not remove temp dir " + dir + ": " + e.message);
         }
      }
      this._tempDirs = [];
   },

   fail: function(message) {
      // Always loud, always throws. The MessageBox is gated on
      // RCAstro.interactive (default false) because it is MODAL: under
      // `PixInsight --automation-mode -r=pipeline.js` there is nobody to
      // click it, so the first CLI failure in a headless run would hang
      // forever with --force-exit never reached. Interactive callers (a
      // tool's dialog-driven main()) opt in by setting RCAstro.interactive =
      // true before invoking the engine.
      console.criticalln("*** " + this.TITLE + ": " + message);
      if (this.interactive)
         (new MessageBox("<p>" + message + "</p>", this.TITLE, StdIcon.Error, StdButton.Ok)).execute();
      throw new Error(message);
   },

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

      let errorMsg   = "";
      let stderrText = "";
      let strayText  = "";
      let buffer     = "";
      let dispatch = function(line) {
         line = line.trim();
         if (line.length == 0 || line.charAt(0) != "{") {
            if (line.length) {
               console.writeln(line);
               // Non-JSON stray text (e.g. a C++ runtime abort/terminate message)
               // observed empirically to arrive via the stdout channel rather than
               // stderr in this ExternalProcess/xvfb harness. Accumulate it as a
               // fallback error source alongside stderrText.
               strayText += (strayText.length ? "\n" : "") + line;
            }
            return;
         }
         let obj = null;
         try { obj = JSON.parse(line); } catch (e) {
            // Looked like JSON (started with "{") but failed to parse — do not
            // vanish it. Log it and fold it into strayText so it can still
            // surface via the stderr/stray fallback in errorMsg on failure.
            console.warningln("rc-astro: could not parse JSON line: " + e.message + " -- " + line);
            strayText += (strayText.length ? "\n" : "") + line;
            return;
         }
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
         if (e.length) {
            console.warningln("rc-astro[stderr]: " + e);
            stderrText += (stderrText.length ? "\n" : "") + e;
         }
      };

      let aborted = false;
      try {
         // NOTE: bare global processEvents(), not CoreApplication.processEvents()
         // (verified not callable in this PI 1.9.4 build — see findBinary() above).
         p.start(cmdLine);
         for (; p.isStarting;) processEvents();
         for (; p.isRunning;) {
            processEvents();
            // A wedged rc-astro process used to busy-spin here forever: nothing
            // checked console.abortRequested, so the PixInsight Abort button did
            // nothing and the only recourse was killing PixInsight itself.
            if (console.abortRequested) {
               aborted = true;
               try { p.kill(); } catch (e) { /* best-effort */ }
               // Let a few more event pumps flush so isRunning can settle after
               // kill() rather than spinning indefinitely if it doesn't.
               for (let guard = 0; p.isRunning && guard < 100; ++guard) processEvents();
               break;
            }
         }
      } catch (e) {
         this.fail("Failed to launch rc-astro: " + e.message);
      }
      if (buffer.length) dispatch(buffer);

      if (aborted)
         return { exit: p.exitCode, ok: false, errorMsg: "rc-astro run aborted by user." };

      let exit = p.exitCode;
      let ok = (exit == 0) && (errorMsg.length == 0);
      // A JSON {"event":"error"} takes precedence (already captured above). If the
      // run failed without ever emitting one (license failure, crash, bad argument
      // caught before JSON init, etc.), fall back to the accumulated stderr text so
      // the real reason isn't lost — only fall back to the generic exit-code message
      // when stderr is also empty.
      if (!ok && errorMsg.length == 0) {
         let trimmedStderr = stderrText.trim();
         let trimmedStray  = strayText.trim();
         let fallback = trimmedStderr.length ? trimmedStderr
                      : trimmedStray.length  ? trimmedStray
                      : "";
         errorMsg = fallback.length ? fallback : ("rc-astro exited with code " + exit);
      }
      return { exit: exit, ok: ok, errorMsg: errorMsg };
   }
};
