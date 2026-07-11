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
