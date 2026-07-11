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
      // NOTE: CoreApplication.processEvents() is documented but is not a
      // callable function in this PixInsight 1.9.4 "Lockhart" build
      // (verified: typeof CoreApplication.processEvents === "undefined").
      // The bare global processEvents() (Global.processEvents, deprecated
      // in the docs but functional here) is used instead.
      for (; p.isStarting;) processEvents();
      for (; p.isRunning;)  processEvents();
      let which = String(p.stdout).trim();
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
      P.newImageColorSpace = PixelMath.SameAsTarget;
      P.newImageSampleFormat = PixelMath.SameAsTarget;
      P.executeOn(targetView);
      resultWindow.forceClose();
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
      console.criticalln("*** " + this.TITLE + ": " + message);
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
         // NOTE: bare global processEvents(), not CoreApplication.processEvents()
         // (verified not callable in this PI 1.9.4 build — see findBinary() above).
         p.start(cmdLine);
         for (; p.isStarting;) processEvents();
         for (; p.isRunning;)  processEvents();
      } catch (e) {
         this.fail("Failed to launch rc-astro: " + e.message);
      }
      if (buffer.length) dispatch(buffer);

      let exit = p.exitCode;
      let ok = (exit == 0) && (errorMsg.length == 0);
      if (!ok && errorMsg.length == 0) errorMsg = "rc-astro exited with code " + exit;
      return { exit: exit, ok: ok, errorMsg: errorMsg };
   }
};
