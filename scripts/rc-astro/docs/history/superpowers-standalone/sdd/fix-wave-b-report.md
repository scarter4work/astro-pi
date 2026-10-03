# Fix Wave B — RCAstroLib.jsh (engine) — CRITICAL/IMPORTANT findings

Scope: `RCAstroLib.jsh` only. No tool scripts or tests were modified except
adding/removing throwaway probes used to verify Fix 5 empirically (removed
before commit; their output is preserved below).

## Fix 5 — empirical verification (done FIRST, per instructions)

**Probe 1** (`test/probe_fix5_saveas.js`, since removed) — does the existing
`saveView()` (calling `win.saveAs()` directly on the source window) re-bind
the window's `filePath`?

```
before saveView: window.filePath = /home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf
saveView wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783812156847-426115/image.xisf
after saveView:  window.filePath = /home/scarter4work/pixinsight-swap/rc-astro-1783812156847-426115/image.xisf
VERDICT: saveAs DID re-bind the window to the temp path.
```

**VERDICT: YES — `ImageWindow.saveAs()` has Save-As semantics and re-binds
`filePath` to the new path.** This confirmed the bug exactly as suspected:
`RCAstro.cleanup()` later deletes that temp directory, so the user's on-screen
window would be left pointing at a deleted file. The next Ctrl+S would
silently stop overwriting the original image file — a serious, silent data-
integrity hazard in a pipeline meant to run unattended across many panels.

**Probe 2** (`test/probe_fix5_dup.js`, since removed) — tried the obvious
alternative, `new ImageWindow(existingWindow)`, to get an independent
duplicate to `saveAs()` instead of the original:

```
dup.isACopy = false
dup.saveAs returned: true
dup.filePath (after saveAs)  = .../dup_test.xisf
original window.filePath (after dup saveAs) = .../dup_test.xisf   <- also rebound!
after dup.forceClose(): original window.filePath =                <- emptied
after dup.forceClose(): original mainView.image still valid, width = 0  <- destroyed
VERDICT: original window.filePath CHANGED -- duplicate approach not safe as implemented.
```

`new ImageWindow(window)` does **not** create an independent copy in this PJSR
build — it aliases the *same* underlying window (`isACopy` was `false`).
`saveAs()`/`forceClose()` on the "duplicate" mutated and then destroyed the
original. This rules out the naive fix.

**Probe 3** (`test/probe_fix5_newwin.js`, since removed) — the canonical
pattern used by bundled PI scripts (`AstroMarkSignatureAdder.js`,
`Halo-B-Gon.js`, etc.): construct a brand-new, off-screen `ImageWindow` sized
to match, `Image.assign()` the pixel data into it inside
`beginProcess`/`endProcess`, then `saveAs()`/`forceClose()` *that* window only:

```
original window.filePath = /home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf
tmpWin.saveAs returned: true
tmpWin.filePath (after saveAs) = .../newwin_test.xisf
original window.filePath (after tmpWin saveAs) = /home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf   <- unchanged
after tmpWin.forceClose(): original window.filePath = /home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf
after tmpWin.forceClose(): original image still valid, width=3744 stdDev=0.01289400470266709   <- unchanged
round-trip file exists: true
VERDICT: new-ImageWindow + assign() approach leaves ORIGINAL window fully intact. SAFE.
```

This is the approach implemented in `saveView()`. FITS keywords are also
copied onto the temp window (`tmpWin.keywords = win.keywords`, verified not to
throw) so the temp file handed to rc-astro isn't silently stripped of
metadata relative to the pre-fix behavior of saving the real window directly.

## What changed, per fix

1. **`applyInPlace()` (CRITICAL)** — `P.executeOn(targetView)`'s Boolean
   return value is now captured. `resultWindow.forceClose()` runs
   unconditionally (success or failure — honors the documented ownership
   contract on every path), and only *after* closing it does the function call
   `this.fail("PixelMath failed to apply the rc-astro result to " + targetView.id)`
   if the apply was declined. A failed apply can no longer look identical to a
   successful one to any caller.

2. **`saveView()` — `View.mainView` (IMPORTANT)** — confirmed against
   `/opt/PixInsight/doc/pjsr/objects/View/View.html`: `View` has no
   `mainView` property (only `isMainView`, `isPreview`, `window`, etc.). The
   function now checks `view.isMainView` up front and calls
   `this.fail("RC-Astro: only main views are supported (previews are not). Target: " + view.id)`
   if it's false, instead of hitting `undefined.window`. Preview support was
   NOT implemented (out of scope, per the brief).

3. **`applyInPlace()` leaked `ImageWindow` (IMPORTANT)** — fixed as part of
   Fix 1: `resultWindow.forceClose()` is now unconditional, so the hidden
   result window is never stranded on the failure path.

4. **`fail()` modal hang (IMPORTANT)** — added `RCAstro.interactive` (default
   `false`) to the `RCAstro` object. `fail()` always does
   `console.criticalln(...)` and always throws; the modal `MessageBox` now
   only appears when `RCAstro.interactive === true`. **Handoff to wave 2**:
   each tool's dialog-driven `main()` branch (the branch that builds and
   `.execute()`s `BXTDialog`/`SXTDialog`/`NXTDialog`) should set
   `RCAstro.interactive = true` before calling `runBXT`/`runSXT`/`runNXT`, so
   interactive users still see the error dialog; the
   `Parameters.isViewTarget` / `Parameters.isGlobalTarget` (process-icon /
   headless) branches should leave it `false` (the default).

5. **`saveView()` — `saveAs` re-binding the user's window (IMPORTANT)** —
   see empirical verification above. Fixed by building an independent
   off-screen `ImageWindow` (matching width/height/channels/bitsPerSample/
   isReal/isColor), copying pixels via `Image.assign()` inside
   `beginProcess`/`endProcess`, copying FITS keywords, then `saveAs()` +
   `forceClose()` on that temp window only. The caller's real window/view is
   never touched.

6. **`runCli()` no abort/timeout (Minor)** — the `for (; p.isRunning;)` poll
   loop now checks `console.abortRequested` on every pump; on abort it calls
   `p.kill()` (best-effort, wrapped in try/catch), pumps a short bounded
   number of additional `processEvents()` calls to let `isRunning` settle,
   and returns `{ exit: p.exitCode, ok: false, errorMsg: "rc-astro run aborted by user." }`
   instead of spinning forever.

7. **`findBinary()` unguarded `p.start()` (Minor)** — the `which rc-astro`
   probe is now wrapped in try/catch; if `ExternalProcess.start()` throws
   (e.g. `/usr/bin/which` missing), a `console.warningln()` is emitted and
   the function falls through to the `File.exists()` candidate loop, which
   was already authoritative.

No public `RCAstro.*` names or signatures changed. `runCli()`'s
`{exit, ok, errorMsg}` shape is unchanged.

## Five-test verification (real runs, `test/run-headless.sh`)

```
--- t_lib_roundtrip ---
binary: /usr/local/bin/rc-astro
PASS t_lib_roundtrip
--- t_lib_runcli ---
progress events seen: true
device used: gpu (NVIDIA GeForce RTX 5070 Ti)
negative-path errorMsg: terminate called after throwing an instance of 'rcastro::Error'
what():  '/home/scarter4work/pixinsight-swap/rc-astro-1783812393476-845363/not_really_xisf.xisf' is not a valid XISF file (bad signature)
PASS t_lib_runcli
--- t_bxt ---
argv[default ss/sn, autoPSF=true]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--ansr","--device","gpu","--output","OUT.xisf"]
argv[autoPSF=false, psfRadius=1.5]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--no-ansr","--nsr","1.50","--device","gpu","--output","OUT.xisf"]
argv[correctOnly=true]: ["IN.xisf","--correct-only","--ash","0.100","--no-ansr","--nsr","1.50","--device","gpu","--output","OUT.xisf"]
argv[mlVersion=4]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--ansr","--ml-version","4","--device","gpu","--output","OUT.xisf"]
PASS t_bxt
--- t_sxt ---
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783812410288-104081/image.xisf
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783812410288-104081/image_starless.xisf
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783812410288-104081/image_starless-stars.xisf
argv[outputStars=false]: ["IN.xisf","--device","gpu","--output","OUT.xisf"]
argv[outputStars=true, unscreen=true]: ["IN.xisf","--output-stars","--unscreen","--device","gpu","--output","OUT.xisf"]
argv[mlVersion=11]: ["IN.xisf","--output-stars","--unscreen","--ml-version","11","--device","gpu","--output","OUT.xisf"]
PASS t_sxt
--- t_nxt ---
argv[useAdvanced=false]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--device","gpu","--output","OUT.xisf"]
argv[useAdvanced=true]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--di","0.500","--dc","0.600","--dhf","0.700","--dlf","0.800","--dihf","0.100","--dilf","0.200","--dchf","0.300","--dclf","0.400","--device","gpu","--output","OUT.xisf"]
argv[mlVersion=3]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--di","0.500","--dc","0.600","--dhf","0.700","--dlf","0.800","--dihf","0.100","--dilf","0.200","--dchf","0.300","--dclf","0.400","--ml-version","3","--device","gpu","--output","OUT.xisf"]
PASS t_nxt
```

All five PASS with the fixed engine.

## Handoff to wave 2 (tool scripts + tests)

- Set `RCAstro.interactive = true` in the dialog-driven branch of each
  tool's `main()` (`RCAstroBXT.js`, `RCAstroSXT.js`, `RCAstroNXT.js`) — the
  branch that builds `BXTDialog`/`SXTDialog`/`NXTDialog` and calls
  `.execute()` — so interactive users still get the modal error MessageBox.
  Leave it at the default `false` for the `Parameters.isViewTarget` /
  `Parameters.isGlobalTarget` (process-icon / scripted / headless) branches.
- None of the existing tests exercised the `applyInPlace()` failure path or
  the preview-view guard in `saveView()` — wave 2 may want dedicated negative
  tests for those (e.g. a locked/read-only target view for Fix 1, a
  `createPreview()`'d view for Fix 2) since this wave was engine-only.

## Concerns

- `saveView()` no longer preserves ICC profile / resolution / astrometric
  solution on the temp file handed to rc-astro (only pixel data + FITS
  keywords now travel across). The original code preserved everything
  because it saved the real window directly. This is very unlikely to affect
  BXT/SXT/NXT (pixel-level ML tools), and no test exercises it, but it's a
  behavior narrowing worth knowing about if a future rc-astro tool ever reads
  those fields from its input file.
- `ExternalProcess.kill()` in Fix 6 is best-effort: if `rc-astro` spawns its
  own subprocesses (e.g. wraps a GPU worker), `kill()` may not reach
  descendants. Not verified against a real wedged process (no reliable way to
  reproduce a hang on demand); verified only that the abort-check code path
  and fallback timeout logic are syntactically/functionally sound via the
  existing five tests, which don't hang.
