#script-id     RCAstroLib

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

   // Fix 7: wall-clock ceiling for a single runCli() invocation, in seconds.
   // console.abortRequested (the mechanism the poll loop below already uses
   // for a user-initiated Abort) never fires under `--automation-mode` --
   // there is no console/user to click Abort -- so a wedged rc-astro process
   // in a headless pipeline used to spin in the `for (; p.isRunning;)` loop
   // forever with no way out. Large mosaic panels are genuinely slow, so this
   // default is intentionally generous, not aggressive; tune per-call via
   // `RCAstro.timeoutSeconds = <n>;` before invoking a tool if a given
   // pipeline needs something different.
   timeoutSeconds: 3600,

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
         // beginProcess(UndoFlag_NoSwapFile): tmpWin is a throwaway window that
         // gets forceClose()'d a few lines below in every case -- writing a
         // full-res swap-file undo snapshot for it is pure wasted disk I/O,
         // doubled per mosaic panel run through BXT/SXT/NXT. UndoFlag_NoSwapFile
         // is defined in <pjsr/UndoFlag.jsh> (already #include-d above) as the
         // PJSR-private value 0xFFFFFFFF -- verified against
         // /opt/PixInsight/include/pjsr/UndoFlag.jsh.
         tmpWin.mainView.beginProcess(UndoFlag_NoSwapFile);
         tmpWin.mainView.image.assign(srcImg);
         tmpWin.mainView.endProcess();
         // Carry FITS keywords along so the temp file handed to the rc-astro
         // CLI (and anything it echoes back) isn't silently stripped of
         // metadata relative to the pre-fix behavior of saving the real window.
         tmpWin.keywords = win.keywords;
         // Fix 2 (root cause): the astrometric solution lives in a separate
         // XISF metadata property, not in the FITS keyword list, so the
         // `tmpWin.keywords = ...` line above never carried it over -- the temp
         // INPUT file handed to the rc-astro CLI silently lost the solution
         // even when the source view had one. Wave C patched only SXT's output
         // side; copy it here too so every tool's temp INPUT file keeps it
         // (verified API against /opt/PixInsight/doc/pjsr/objects/ImageWindow/
         // ImageWindow.html: hasAstrometricSolution / copyAstrometricSolution).
         if (win.hasAstrometricSolution) tmpWin.copyAstrometricSolution(win);
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
      //
      // Fix 4: the close must genuinely happen on every path, including
      // P.executeOn() THROWING rather than returning false. The previous
      // sequential `let ok = executeOn(); resultWindow.forceClose();` skipped
      // the close entirely if executeOn() threw -- wrap in try/finally so the
      // close is unconditional, and closed exactly once (not also on a second
      // pass through some other cleanup path).
      let ok = false;
      try {
         ok = P.executeOn(targetView);
      } finally {
         resultWindow.forceClose();
      }
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

   // Parse one NDJSON line from rc-astro's --json stdout stream, updating
   // `state` ({errorMsg, strayText}) in place and calling onEvent(obj) for
   // every successfully-parsed event. Split out of runCli() so this is
   // independently unit-testable (test/t_lib_dispatch_splice.js) without
   // spawning a real rc-astro process.
   //
   // Why this exists / hardening rationale: PI's ExternalProcess merges a
   // child process's stderr into the SAME stdout stream, interleaved in
   // write order. This is UNIVERSAL to PI's ExternalProcess -- true in the
   // normal GUI and under --automation-mode alike, not an artifact of
   // headless/xvfb runs (verified: see the review at
   // .superpowers/sdd/2026-09-25-pi-copilot-image-journey/task-keyring-review.md,
   // "RCAstroLib" section). onStandardErrorDataAvailable below never fires
   // and readStandardError()/this.stderr is always empty as a result --
   // stderrText stays empty in practice; it is kept only as a defensive
   // no-op in case a future PI build ever does deliver stderr separately.
   //
   // Empirically (2026-09-26, outside PI): rc-astro 0.9.10 writes each NDJSON
   // event via a single atomic write() syscall including its trailing
   // newline -- verified with `strace -f -tt -e trace=write` against a real,
   // reliably-reproducing crash (`rc-astro bxt --benchmark N --device
   // gpu|cpu` SIGABRTs in this environment during model init with a
   // libstdc++ "terminate called ... rcastro::Error ... ml-onnx.cpp:877"
   // message on stderr). Across 7 runs the two preceding stdout JSON lines
   // were each one complete write(), and the stderr message -- itself split
   // across 6 separate write() calls by libstdc++'s terminate handler --
   // always landed strictly after both stdout writes, never mid-line: no
   // splice was observed. But that repro dies during early model init,
   // before rc-astro's real (likely multi-threaded, given ONNX Runtime)
   // progress-emission code path ever runs, so a worker-thread stderr write
   // landing between two stdout write() calls mid-line cannot be ruled out
   // from this evidence. Handle it defensively rather than trust it can't
   // happen: a line with leading non-JSON bytes ahead of its first '{' is
   // parsed from that '{' onward, and the stray prefix is folded into
   // strayText -- never dropped silently.
   _dispatchLine: function(rawLine, state, onEvent) {
      let line = rawLine.trim();
      if (line.length == 0) return;

      let brace = line.indexOf("{");
      if (brace < 0) {
         // No JSON object anywhere on this line -- pure stray text (e.g. a
         // C++ runtime abort/terminate message, or a plain log line).
         console.writeln(line);
         state.strayText += (state.strayText.length ? "\n" : "") + line;
         return;
      }

      let prefix   = line.substring(0, brace);
      let jsonPart = line.substring(brace);
      if (prefix.length) {
         // Leading bytes ahead of the first '{' -- e.g. a spliced-in stderr
         // fragment. Never drop it: fold into strayText so it still surfaces
         // via the stderr/stray fallback in errorMsg on failure.
         console.writeln(prefix);
         state.strayText += (state.strayText.length ? "\n" : "") + prefix;
      }

      let obj = null;
      try { obj = JSON.parse(jsonPart); } catch (e) {
         // Looked like JSON (contained "{") but failed to parse — do not
         // vanish it. Log it and fold it into strayText so it can still
         // surface via the stderr/stray fallback in errorMsg on failure.
         console.warningln("rc-astro: could not parse JSON line: " + e.message + " -- " + jsonPart);
         state.strayText += (state.strayText.length ? "\n" : "") + jsonPart;
         return;
      }
      if (obj) {
         if (obj.event == "error")   { state.errorMsg = obj.message || obj.text || JSON.stringify(obj); console.criticalln("rc-astro: " + state.errorMsg); }
         else if (obj.event == "warning") console.warningln("rc-astro: " + (obj.message || obj.text || ""));
         if (onEvent) onEvent(obj);
      }
   },

   // Run rc-astro <tool> with args; parse --json NDJSON stream via onEvent.
   runCli: function(tool, argsArray, onEvent) {
      let bin = this.findBinary();
      if (!bin) this.fail("rc-astro CLI not found at /usr/local/bin/rc-astro or on PATH.");

      // Build a quoted command line: quote tokens that are paths / contain spaces.
      let quote = function(s){ return /[^A-Za-z0-9_.\-\/]/.test(s) ? '"' + s + '"' : s; };
      // Run the child WITHOUT PixInsight's LD_LIBRARY_PATH. PixInsight.sh exports
      // LD_LIBRARY_PATH=/opt/PixInsight/bin/lib:/opt/PixInsight/bin and every
      // ExternalProcess inherits it. Since PI 1.9.x that directory ships PI's own
      // libonnxruntime.so.1 (1.25.1, for the MachineLearning module). rc-astro
      // finds its bundled onnxruntime through RUNPATH=$ORIGIN, which
      // LD_LIBRARY_PATH overrides, so the inherited path made rc-astro load PI's
      // copy and die before main(): "libonnxruntime.so.1: version `VERS_1.23.2'
      // not found". PixInsight.sh replaces (does not extend) any user value, so
      // unsetting it restores exactly what rc-astro gets from a normal shell.
      // ExternalProcess runs no shell, hence env(1) rather than `VAR= cmd`.
      let parts = ["/usr/bin/env", "-u", "LD_LIBRARY_PATH", quote(bin), "--no-banner", tool];
      for (let i = 0; i < argsArray.length; ++i) parts.push(quote(String(argsArray[i])));
      parts.push("--json", "--overwrite");
      let cmdLine = parts.join(" ");
      console.writeln("running: " + cmdLine);

      let self       = this;
      let state      = { errorMsg: "", strayText: "" };
      let stderrText = "";
      let buffer     = "";
      let dispatch = function(line) { self._dispatchLine(line, state, onEvent); };

      let p = new ExternalProcess;
      p.onStandardOutputDataAvailable = function() {
         buffer += String(this.stdout);
         let nl;
         while ((nl = buffer.indexOf("\n")) >= 0) { dispatch(buffer.substring(0, nl)); buffer = buffer.substring(nl + 1); }
      };
      // DEAD CODE on this platform, kept as a harmless defensive no-op: PI's
      // ExternalProcess merges the child's stderr into the stdout stream
      // (see the full explanation above _dispatchLine()), so this callback
      // never fires and `this.stderr` is always empty -- stderrText below
      // stays "" for the life of the run. The real fallback error text on
      // this platform comes from state.strayText (populated by
      // _dispatchLine() from the merged stdout channel), not from here.
      p.onStandardErrorDataAvailable = function() {
         let e = String(this.stderr).trim();
         if (e.length) {
            console.warningln("rc-astro[stderr]: " + e);
            stderrText += (stderrText.length ? "\n" : "") + e;
         }
      };

      let aborted = false;
      let timedOut = false;
      let startTime = Date.now();
      let timeoutMs = this.timeoutSeconds * 1000;
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
            // Fix 7: console.abortRequested is a no-op under --automation-mode
            // (nobody is present to press Abort), so a headless pipeline needs
            // its own ceiling. Date.now() is a real working elapsed-time
            // mechanism in this PJSR build (already used by tempDir() above).
            if (Date.now() - startTime > timeoutMs) {
               timedOut = true;
               try { p.kill(); } catch (e) { /* best-effort */ }
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

      if (timedOut)
         return { exit: p.exitCode, ok: false,
                  errorMsg: "rc-astro timed out after " + this.timeoutSeconds + "s" };

      let exit = p.exitCode;
      let ok = (exit == 0) && (state.errorMsg.length == 0);
      // A JSON {"event":"error"} takes precedence (already captured above). If the
      // run failed without ever emitting one (license failure, crash, bad argument
      // caught before JSON init, etc.), fall back to the accumulated stderr text so
      // the real reason isn't lost — only fall back to the generic exit-code message
      // when stderr is also empty.
      if (!ok && state.errorMsg.length == 0) {
         let trimmedStderr = stderrText.trim();
         let trimmedStray  = state.strayText.trim();
         let fallback = trimmedStderr.length ? trimmedStderr
                      : trimmedStray.length  ? trimmedStray
                      : "";
         state.errorMsg = fallback.length ? fallback : ("rc-astro exited with code " + exit);
      }
      return { exit: exit, ok: ok, errorMsg: state.errorMsg };
   }
};
