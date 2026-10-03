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

