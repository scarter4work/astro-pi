#feature-id    RC-Astro > StarXTerminator (CLI)
#feature-info  Runs the GPU-accelerated rc-astro StarXTerminator; starless (+ optional stars) to new windows.

#include <pjsr/Sizer.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
// NOTE: no <pjsr/Label.jsh> — Label is a native global; that header does not exist in this build.
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
   // CONFIRMED via Task 4 Step 2 empirical run (see task-4 report): the CLI
   // derives the stars filename from the starless OUTPUT basename using a
   // HYPHEN, not an underscore: "<outP-without-.xisf>-stars.xisf".
   let starsP = dir + "/" + id + "_starless-stars.xisf";
   let temps = [inP, outP, starsP];
   try {
      let r = RCAstro.runCli("sxt", SXTParams.buildArgs(inP, outP), null);
      if (!r.ok) { RCAstro.fail("StarXTerminator failed: " + r.errorMsg); return; }
      let starless = RCAstro.importResult(outP, id + "_starless");
      RCAstro.newWindow(starless, id + "_starless");
      if (SXTParams.outputStars) {
         if (!File.exists(starsP)) {
            RCAstro.fail("Stars image was requested but rc-astro did not produce the " +
                         "expected output: " + starsP);
            return;
         }
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
// RCAstroSXT_TESTING is set by test/t_sxt.js before #include-ing this file so
// it can call runSXT()/SXTParams directly without launching the (headless-
// incompatible) interactive dialog. Normal launches (Scripts menu, process
// icon, #feature-id) never define this global, so main() runs as usual.
if (typeof RCAstroSXT_TESTING === "undefined") {
   main();
}
