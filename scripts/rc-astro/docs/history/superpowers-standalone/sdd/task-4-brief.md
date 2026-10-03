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

