# Fix wave D report

Final fix wave closing the remaining eight Minor/Important findings from the
whole-branch review (all eight original Critical/Important findings were
already closed by waves prior to this one).

## Fix 1 (IMPORTANT) — correct-only mode was still broken; CLI hard-rejects our argv

`RCAstroBXT.js` `BXTParams.buildArgs()`: the `--ansr` / `--no-ansr --nsr` push
used to happen **unconditionally**, outside the `correctOnly` branch — so
`--correct-only` runs still emitted `--ansr` (or `--no-ansr --nsr <n>`), both
of which the real `rc-astro` 0.9.10 CLI rejects when combined with
`--correct-only`.

**Fix**: moved the `--ansr`/`--no-ansr --nsr` push inside the `else` branch of
`if (this.correctOnly)`, so `correctOnly=true` now emits **only**
`--correct-only` (plus `--ml-version`/`--device`/`--output`).

**Real argv now emitted** (from the actual `t_bxt` run below):
```
argv[correctOnly=true]: ["IN.xisf","--correct-only","--device","gpu","--output","OUT.xisf"]
```
No `--ss`, `--sn`, `--ash`, `--ansr`, `--no-ansr`, or `--nsr` present.

**Dialog**: `correctOnly.onCheck` now also disables `autoPSF` and `nsr`
(`self.autoPSF.enabled = !c; self.nsr.enabled = !c && !BXTParams.autoPSF;`),
and the same disabled-state logic is applied at construction time (mirroring
the existing `ss`/`sn`/`ash` pattern), so a user can no longer reach the
CLI-rejected state via the UI.

**Test**: `test/t_bxt.js` now (a) asserts the correct-only argv contains none
of `--ash --ansr --no-ansr --nsr` (previously it only checked `--ss`/`--sn`/
`--ash`), deliberately leaving `autoPSF=false; psfRadius=1.5` set from the
prior block so the assertion proves suppression is unconditional on
`correctOnly`, not just default field values; and (b) **actually executes**
`runBXT(vCO)` with `correctOnly=true` against the real CLI/GPU and asserts the
target view's pixels changed. This path had never been run end-to-end before.

**Proof of successful real-CLI run** (from the actual `t_bxt` log):
```
correct-only end-to-end run: PASS (stdDev before=0.01289400470266709 after=0.013834526797147877)
```

## Fix 2 (Minor) — `saveView` now carries the astrometric solution on the INPUT side (root cause)

`RCAstroLib.jsh` `saveView()`: added
`if (win.hasAstrometricSolution) tmpWin.copyAstrometricSolution(win);` before
`saveAs()`, verified against
`/opt/PixInsight/doc/pjsr/objects/ImageWindow/ImageWindow.html`
(`hasAstrometricSolution` / `copyAstrometricSolution` both confirmed present).
SXT's existing output-side `copyAstrometricSolution()` calls were left in
place (belt and braces) — `t_sxt`'s Fix D assertions (`starlessSolved.
hasAstrometricSolution`, `starsSolved.hasAstrometricSolution`) still pass.

## Fix 3 (Minor) — `closeWindowById()` no longer silently destroys unsaved user work

`RCAstroSXT.js` `closeWindowById()` used to unconditionally `forceClose()` a
pre-existing `<id>_starless`/`<id>_stars` window, destroying any unsaved edits
with no prompt and no undo.

**Fix**: check `ImageWindow.isModified` (verified present in the ImageWindow
docs) before closing. If the existing window has unsaved modifications, skip
the close (log a `console.warningln()`) and let PixInsight's own uniquify
behavior give the new result a different id (e.g. `<id>_starless1`) instead of
destroying the user's window.

**Empirically verified the "let PixInsight uniquify" plan is actually safe**
before committing to it: a throwaway probe (`ImageWindow.mainView.id =
"already-taken-id"`) confirmed this **does not throw** — it silently
auto-uniquifies (`w2.mainView.id` became `"taken_id1"`), matching the
existing "Fix H" comment's expectation. Probe script and its log were deleted
after the check; not part of the committed diff.

## Fix 4 (Minor) — `applyInPlace`'s `forceClose()` now in a genuine `finally`

`RCAstroLib.jsh` `applyInPlace()`: `let ok = P.executeOn(targetView);
resultWindow.forceClose();` used to skip the close entirely if `executeOn()`
threw rather than returning `false`. Wrapped in `try { ok = P.executeOn(...); }
finally { resultWindow.forceClose(); }` so the close happens exactly once, on
every path.

## Fix 5 (Minor) — `beginProcess(UndoFlag_NoSwapFile)`

`RCAstroLib.jsh` `saveView()`: `tmpWin.mainView.beginProcess()` (default
flags) changed to `tmpWin.mainView.beginProcess(UndoFlag_NoSwapFile)`.
Verified the constant against `/opt/PixInsight/include/pjsr/UndoFlag.jsh`:
`#define UndoFlag_NoSwapFile 0xFFFFFFFF` (PJSR-private value, already
`#include <pjsr/UndoFlag.jsh>`'d at the top of the file). `tmpWin` is
forceClose()'d unconditionally a few lines later in every case, so the
swap-file undo snapshot was pure wasted I/O.

## Fix 6 (Minor) — removed dead/misleading guard

`RCAstroBXT.js` / `RCAstroNXT.js` / `RCAstroSXT.js` `main()`: removed
`if (XParams.targetView === undefined)` — confirmed `load()` never sets
`targetView` in any of the three (no `Parameters.has("targetView")` handling
exists), so the condition was always true. Replaced with an unconditional
assignment from `ImageWindow.activeWindow`.

## Fix 7 (Minor) — wall-clock timeout in `runCli`

`RCAstroLib.jsh`: added `RCAstro.timeoutSeconds` (default `3600`, settable
per-call, e.g. `RCAstro.timeoutSeconds = 60;` before invoking a tool). The
`for (; p.isRunning;)` poll loop now also checks `Date.now() - startTime >
timeoutMs` alongside the existing `console.abortRequested` check; on timeout
it kills the process and returns
`{ok:false, errorMsg:"rc-astro timed out after Ns"}`.
`Date.now()` was already in use elsewhere in this file (`tempDir()`),
confirming it's a working elapsed-time mechanism in this PJSR build — no
separate probe needed.

## Fix 8 (Minor) — documented that GLOBAL execution is GUI-only

Added a "Headless / pipeline use" section to `README.md` stating plainly:
drive these tools via a **view target**
(`Parameters.isViewTarget` / `executeOn(view)`), never `executeGlobal()` on a
saved process icon, for headless/pipeline work — `executeGlobal()` runs the
`isGlobalTarget` branch, which always opens the interactive dialog (per Fix
C), and PJSR has no API to detect automation mode from inside a script.

## Full five-test suite — real output

```
--- t_lib_roundtrip ---
binary: /usr/local/bin/rc-astro
PASS t_lib_roundtrip
--- t_lib_runcli ---
progress events seen: true
device used: gpu (NVIDIA GeForce RTX 5070 Ti)
negative-path errorMsg: terminate called after throwing an instance of 'rcastro::Error'
what():  '/home/scarter4work/pixinsight-swap/rc-astro-1783814287511-978147/not_really_xisf.xisf' is not a valid XISF file (bad signature)
PASS t_lib_runcli
--- t_bxt ---
argv[default ss/sn, autoPSF=true]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--ansr","--device","gpu","--output","OUT.xisf"]
argv[autoPSF=false, psfRadius=1.5]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--no-ansr","--nsr","1.50","--device","gpu","--output","OUT.xisf"]
argv[correctOnly=true]: ["IN.xisf","--correct-only","--device","gpu","--output","OUT.xisf"]
correct-only end-to-end run: PASS (stdDev before=0.01289400470266709 after=0.013834526797147877)
argv[mlVersion=4]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--ansr","--ml-version","4","--device","gpu","--output","OUT.xisf"]
PASS t_bxt
--- t_sxt ---
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783814307591-823074/image.xisf
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783814307591-823074/image_starless.xisf
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783814307591-823074/image_starless-stars.xisf
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

All five PASS. No regressions from waves A/B/C's fixes (Fix D astrometric
solution assertions in `t_sxt`, Fix H id-collision assertions in `t_sxt`, and
the negative-path/device-info assertions in `t_lib_runcli` all still hold).
