#feature-id    RC-Astro > NoiseXTerminator (CLI)
#feature-info  Runs the GPU-accelerated rc-astro NoiseXTerminator on the target view.

#include <pjsr/Sizer.jsh>
#include <pjsr/NumericControl.jsh>
#include <pjsr/StdButton.jsh>
#include <pjsr/StdIcon.jsh>
// NOTE: no <pjsr/Label.jsh> — Label is a native global; that header does not exist in this build.
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
   } finally {
      RCAstro.cleanup([inP, outP]);
   }
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

   // Advanced section — collapsed by default. Following the canonical PJSR
   // pattern used by bundled scripts (AdP/MosaicPlanner.js, AdP/AlignByCoordinates.js):
   // create the SectionBar and its Control, wire them with setSection(), then
   // explicitly hide()/show() the control per the desired initial state.
   // SectionBar.setSection() hooks the control's onShow/onHide to keep the
   // bar's collapse/expand icon in sync; setting `.visible` directly would
   // bypass those hooks, so hide()/show() are used instead (matching the
   // reference scripts) rather than the brief's draft `.visible = ...` assignment.
   this.advBar = new SectionBar(this, "Advanced (per-scale)");
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
   this.advBar.setSection(this.advCtl);
   if (NXTParams.useAdvanced) { this.advCtl.adjustToContents(); this.advCtl.show(); }
   else this.advCtl.hide();
   this.advBar.onToggleSection = function(bar, toggleBegin) {
      // Track "section expanded" as "use advanced" once the toggle animation
      // completes (toggleBegin == false), matching bar.isExpanded()'s
      // definition (hasSection() && section.visible).
      if (!toggleBegin) NXTParams.useAdvanced = bar.isExpanded();
   };

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
// RCAstroNXT_TESTING is set by test/t_nxt.js before #include-ing this file so
// it can call runNXT()/NXTParams directly without launching the (headless-
// incompatible) interactive dialog. Normal launches (Scripts menu, process
// icon, #feature-id) never define this global, so main() runs as usual.
if (typeof RCAstroNXT_TESTING === "undefined") {
   main();
}
