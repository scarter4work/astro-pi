#feature-id    RC-Astro > BlurXTerminator (CLI)
#feature-info  Runs the GPU-accelerated rc-astro BlurXTerminator on the target view.

#include <pjsr/Sizer.jsh>
#include <pjsr/FrameStyle.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
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
      if (this.correctOnly) {
         a.push("--correct-only");
         // --ash is invalid together with --correct-only: verified empirically
         // (`rc-astro bxt --correct-only --ash 0.100` --> {"event":"error",
         // "message":"--correct-only forces --ash to 0, which conflicts with
         // the value you gave; omit --ash"}). Never emit it in this mode.
      } else {
         a.push("--ss", format("%.3f", this.sharpenStars),
                "--sn", format("%.3f", this.sharpenNonstellar));
         a.push("--ash", format("%.3f", this.adjustHalos));
      }
      if (this.autoPSF) a.push("--ansr");
      else a.push("--no-ansr", "--nsr", format("%.2f", this.psfRadius));
      if (this.mlVersion != 0) a.push("--ml-version", String(this.mlVersion));
      a.push("--device", this.device, "--output", outPath);
      return a;
   }
};

function runBXT(view) {
   if (view == null || view.isNull) { RCAstro.fail("No target view."); return; }
   // Vars declared before the try, but tempDir()/saveView() are CALLED inside
   // it: if saveView() throws (disk full, read-only TMPDIR, non-main-view
   // guard, ...) the temp directory tempDir() just created must still be
   // swept up by the `finally` below, not stranded.
   let dir = null, inP = null, outP = null;
   try {
      dir = RCAstro.tempDir();
      inP = RCAstro.saveView(view, dir);
      outP = dir + "/" + view.id + "_bxt.xisf";
      let r = RCAstro.runCli("bxt", BXTParams.buildArgs(inP, outP), null);
      if (!r.ok) { RCAstro.fail("BlurXTerminator failed: " + r.errorMsg); return; }
      let out = RCAstro.importResult(outP, view.id + "_bxt_tmp");
      RCAstro.applyInPlace(out, view);   // applyInPlace closes `out` — do NOT forceClose it
   } finally {
      // cleanup() null-guards each path entry, and always sweeps the temp
      // dir(s) recorded by tempDir() regardless of what's passed here.
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
   this.correctOnly.onCheck = function(c){ BXTParams.correctOnly = c; self.ss.enabled = !c; self.sn.enabled = !c; self.ash.enabled = !c; };
   // Initialize ALL dependent enabled-states from the loaded param values at
   // construction, not just when the user toggles a checkbox interactively --
   // required now that Fix C opens this dialog pre-populated from a saved
   // process icon's parameters (previously only reachable via the Scripts
   // menu, where params were always defaults and this never mattered).
   // --ash is invalid together with --correct-only (see buildArgs()), so the
   // slider must start disabled whenever a loaded/default correctOnly is true.
   this.ss.enabled = !BXTParams.correctOnly;
   this.sn.enabled = !BXTParams.correctOnly;
   this.ash.enabled = !BXTParams.correctOnly;

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
   // isViewTarget: a saved-instance drop targeting a specific view (the
   // headless/automation path, e.g. a scripted mosaic pipeline). Run
   // immediately, no dialog -- RCAstro.interactive stays at its default
   // (false), so a failure never pops a modal MessageBox that nothing is
   // present to dismiss.
   if (Parameters.isViewTarget) { BXTParams.load(); runBXT(Parameters.targetView); return; }
   // isGlobalTarget: double-clicking a saved process icon. Previously this
   // ran immediately against ImageWindow.activeWindow -- silently and
   // destructively rewriting whatever window happened to be active (e.g. the
   // user's master flat), with settings that could never be reviewed or
   // edited because the dialog was only reachable from the Scripts menu
   // (where params are always defaults). Matches the bundled-script
   // convention (e.g. MaskMerge.js): load() the saved params, then fall
   // through to show the dialog pre-populated -- never auto-run.
   if (Parameters.isGlobalTarget) BXTParams.load();
   if (BXTParams.targetView === undefined)
      BXTParams.targetView = ImageWindow.activeWindow.isNull ? undefined : ImageWindow.activeWindow.mainView;
   // Dialog-driven branch: the user is present, so let fail() show its modal
   // MessageBox in addition to the console log.
   RCAstro.interactive = true;
   let d = new BXTDialog();
   d.doRun = false;
   if (d.execute() && d.doRun) runBXT(BXTParams.targetView);
}
// RCAstroBXT_TESTING is set by test/t_bxt.js before #include-ing this file so
// it can call BXTParams.buildArgs() directly without launching the (headless-
// incompatible) interactive dialog. Normal launches (Scripts menu, process
// icon, #feature-id) never define this global, so main() runs as usual.
if (typeof RCAstroBXT_TESTING === "undefined") {
   main();
}
