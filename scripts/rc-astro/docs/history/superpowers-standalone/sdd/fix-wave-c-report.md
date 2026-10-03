# Fix Wave C — RCAstroBXT.js / RCAstroSXT.js / RCAstroNXT.js + tests

Scope: the three tool scripts and their tests only. `RCAstroLib.jsh` was
**not** modified (no fix in this wave required it).

## Fix D — empirical probe (done FIRST, per instructions)

Two throwaway probes were used and removed before commit (their output is
preserved here, matching the Wave B convention for probe_fix5_*.js).

**Probe 1 finding**: the test fixture normally used by these tests
(`/home/scarter4work/astro_work/cygnus/gxp/panel_1-1.xisf`) has
`hasAstrometricSolution = false` and 0 FITS keywords — it can't exercise this
fix at all. A search across other local astro data found
`/home/scarter4work/astro_work/ic4604/registered/Light_IC 4604_2-4_..._d_r.xisf`,
a real plate-solved/registered frame with `hasAstrometricSolution = true` and
106 FITS keywords (RA/DEC/SIP distortion terms/etc.) but, notably, **no**
`CTYPE`/`CRVAL`/`CD1_1`-style WCS keywords in its FITS keyword list at all —
confirming the astrometric solution lives in a separate XISF metadata
property, not encoded in the FITS keyword set.

**Probe 2 (BEFORE the fix)**, `test/probe_fixD_astrometric.js` (removed),
ran the real `runSXT()` against that fixture:

```
source window.hasAstrometricSolution: true
source window.keywords.length: 106
starless window found: true
starless.hasAstrometricSolution: false      <-- LOST
starless.keywords.length: 110
keyword names DROPPED (in source, not in starless): []
keyword names EXTRA (in starless, not in source): ["HISTORY"]
stars window found: true
stars.hasAstrometricSolution: false          <-- LOST
```

**VERDICT (BEFORE)**: FITS keywords round-trip perfectly (0 dropped, only an
expected extra `HISTORY` entry from the XISF save/open cycle) — Wave B's
`saveView()` keyword-copy already covers that. But
`ImageWindow.hasAstrometricSolution` is **false** on both new windows: the
astrometric solution is a distinct XISF metadata property that
`saveView()`'s `tmpWin.keywords = win.keywords` never touches, and rc-astro's
own output (built from that keyword-only temp file) never had one to begin
with. **This is exactly the failure the brief predicted: SXT's new windows
are unusable for plate-solving/mosaic workflows without it.**

**Fix**: in `runSXT()`, after creating each new window (`starless`, and
`stars` if requested), guard on `view.window.hasAstrometricSolution` and call
`newWin.copyAstrometricSolution(view.window)` — the pattern used by the
bundled `AutoDBE.js` (`ImageWindow.copyAstrometricSolution(ImageWindow)`,
confirmed against `/opt/PixInsight/doc/pjsr/objects/ImageWindow/ImageWindow.html`).

**Probe 2 (AFTER the fix)**, same probe, re-run:

```
source window.hasAstrometricSolution: true
starless window found: true
starless.hasAstrometricSolution: true       <-- FIXED
stars window found: true
stars.hasAstrometricSolution: true          <-- FIXED
```

**VERDICT (AFTER)**: fixed. Both new windows now carry the source's
astrometric solution (as well as its FITS keywords, which already worked).

## Fix G — CLI determination

`/usr/local/bin/rc-astro bxt --help` documents `--correct-only` and `--ash`
as independent flags with no stated conflict. A real invocation settled it:

```
$ rc-astro bxt IN.xisf --correct-only --ash 0.100 --device gpu --output OUT.xisf --overwrite --json --no-banner
{"event":"error","message":"--correct-only forces --ash to 0, which conflicts with the value you gave; omit --ash"}
```

and, confirming `--correct-only` alone (no `--ash`) runs cleanly to
completion (`{"event":"status","phase":"complete",...}`).

**VERDICT**: `--ash` is **not valid** with `--correct-only` — any non-zero
value is rejected outright (this was a live bug: `BXTParams.buildArgs()`
always pushed `--ash` unconditionally, so any saved instance with
`correctOnly=true` and a non-zero `adjustHalos` would make BXT fail every
time). Fixed by moving the `--ash` push inside the `else` branch of
`buildArgs()` (never emitted under `correct-only`), and disabling the halo
slider (`this.ash`) whenever `correctOnly` is checked (both at construction
and in the `onCheck` handler).

## What changed, per fix

1. **Fix A — `RCAstro.interactive`**: each tool's `main()` now sets
   `RCAstro.interactive = true` only in the dialog-driven branch(es), right
   before `d.execute()`. The `isViewTarget` (headless/automation) branch is
   untouched and stays at the engine's default `false`.

2. **Fix B — stranded temp dir on early failure**: in `runBXT`/`runSXT`/
   `runNXT`, `dir`/`inP`/`outP`(/`starsP` for SXT) are now declared `null`
   before the `try`, and `RCAstro.tempDir()`/`RCAstro.saveView()` are called
   *inside* it. If `saveView()` throws, the `finally { RCAstro.cleanup(...) }`
   now always runs and sweeps the directory `tempDir()` already registered
   internally — regardless of whether `saveView()` got far enough to return
   a file path. `cleanup()`'s own `paths[i] &&` guard already tolerates the
   `null` entries this produces, so no extra filtering was needed.

3. **Fix C — `isGlobalTarget` destructive silent run**: previously
   `isGlobalTarget` ran immediately against `ImageWindow.activeWindow`, with
   no prompt and no way to ever review/edit a saved instance's settings
   (dialog only reachable from the Scripts menu, where params are always
   defaults). Now, matching the bundled `MaskMerge.js` convention:
   `isGlobalTarget` calls `Params.load()` and falls through to show the
   dialog (pre-populated from the loaded params, since the dialog
   constructor reads directly from `*Params.*`) — never auto-runs. The
   `isViewTarget` branch (the one the headless pipeline needs) is unchanged:
   immediate run, no dialog, `return` before reaching the dialog code.

4. **Fix D — SXT astrometric solution**: see probe above.
   `starless.copyAstrometricSolution(view.window)` / same for `stars`,
   guarded on `view.window.hasAstrometricSolution`.

5. **Fix E — tests must call the real entry points**: `test/t_bxt.js` and
   `test/t_nxt.js` now set `BXTParams`/`NXTParams` and call the real
   `runBXT(v)` / `runNXT(v)` (previously they re-implemented
   tempDir→saveView→runCli→importResult→applyInPlace inline, bypassing the
   actual shipped glue). `test/t_sxt.js` already called the real `runSXT()`
   and needed no change there; it also gained two new regression checks (Fix
   D and Fix H, added below) inline rather than as separate files, since the
   five-test loop the harness runs is fixed at five names.

6. **Fix F — BXT dialog enabled-state**: `this.ss.enabled`, `this.sn.enabled`,
   and `this.ash.enabled` are now all initialized from `BXTParams.correctOnly`
   at construction time (previously only wired in the `onCheck` handler, so a
   dialog opened pre-populated via Fix C with a loaded `correctOnly=true`
   would show the sliders enabled when they shouldn't be). SXT's
   `unscreenCB.enabled = SXTParams.outputStars` was already correct at
   construction — verified, no change needed.

7. **Fix G — `--ash` under `--correct-only`**: see CLI determination above.
   `buildArgs()` now only pushes `--ash` in the non-`correctOnly` branch; the
   halo slider's enabled state is tied to `!correctOnly` (Fix F).

8. **Fix H — SXT re-run id collision**: added `closeWindowById(id)` in
   `RCAstroSXT.js`; `runSXT()` calls it for `<id>_starless` (and
   `<id>_stars` when requested) immediately before `importResult()`, so a
   re-run on the same view deliberately replaces the previous result's
   windows instead of colliding with them. Verified via a throwaway probe
   (`test/probe_fixH_rerun.js`, removed before commit): calling `runSXT(v)`
   twice in a row on the same view, before the fix, is exactly the scenario
   the brief describes; after the fix, the second run cleanly replaces both
   windows and no `_starless1`/`_stars1` uniquified windows appear. This
   check is now also a permanent part of `test/t_sxt.js` (Part 2b).

## Test changes

- `test/t_bxt.js`: headless-path section now sets `BXTParams` and calls the
  real `runBXT(v)`, asserting the target view's pixels actually changed.
  Added an assertion that `--ash` is absent from `buildArgs()`'s output when
  `correctOnly=true` (Fix G regression guard) — this specific combination
  (`correctOnly=true`, `adjustHalos=0.10` left over from an earlier assertion
  block) is exactly what used to make a real `rc-astro bxt --correct-only`
  invocation fail.
- `test/t_nxt.js`: same treatment — real `runNXT(v)` call replaces the inline
  reimplementation.
- `test/t_sxt.js`: added two new inline parts (no new files, since the
  five-test loop is fixed):
  - **Part 2b** (Fix H): re-runs `runSXT(v)` a second time on the same view
    immediately after Part 2's first run, before closing those windows;
    asserts both windows are still present under the same ids and no
    `_starless1`/`_stars1` uniquified window exists.
  - **Part 2c** (Fix D): opens the IC4604 registered frame (the only local
    fixture with a real astrometric solution), runs `runSXT()` on it, and
    asserts both the `_starless` and `_stars` windows report
    `hasAstrometricSolution === true`.

## Five-test verification (real runs, `test/run-headless.sh`)

```
--- t_lib_roundtrip ---
binary: /usr/local/bin/rc-astro
PASS t_lib_roundtrip
--- t_lib_runcli ---
progress events seen: true
device used: gpu (NVIDIA GeForce RTX 5070 Ti)
negative-path errorMsg: terminate called after throwing an instance of 'rcastro::Error'
what():  '/home/scarter4work/pixinsight-swap/rc-astro-1783813402725-255747/not_really_xisf.xisf' is not a valid XISF file (bad signature)
PASS t_lib_runcli
--- t_bxt ---
argv[default ss/sn, autoPSF=true]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--ansr","--device","gpu","--output","OUT.xisf"]
argv[autoPSF=false, psfRadius=1.5]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--no-ansr","--nsr","1.50","--device","gpu","--output","OUT.xisf"]
argv[correctOnly=true]: ["IN.xisf","--correct-only","--no-ansr","--nsr","1.50","--device","gpu","--output","OUT.xisf"]
argv[mlVersion=4]: ["IN.xisf","--ss","0.250","--sn","0.900","--ash","0.100","--ansr","--ml-version","4","--device","gpu","--output","OUT.xisf"]
PASS t_bxt
--- t_sxt ---
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783813419887-15589/image.xisf
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783813419887-15589/image_starless.xisf
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783813419887-15589/image_starless-stars.xisf
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

All five PASS. Note `argv[correctOnly=true]` no longer contains `--ash` —
direct visible evidence of the Fix G regression guard working (that argv is
built with `adjustHalos = 0.10` still set from an earlier assertion in the
same test, which pre-fix would have produced `--ash 0.100` and a real CLI
run would have rejected it).

No leaked temp directories after the full run
(`ls ~/pixinsight-swap | grep -c rc-astro-` → `0`).

## Concerns / handoff

- `RCAstroLib.jsh` was not touched. No fix in this wave required it.
- Fix D's `copyAstrometricSolution` is only wired into `runSXT()` (the only
  tool that creates new windows). BXT/NXT apply in place via `applyInPlace()`
  onto the caller's real view, which already owns its own metadata
  untouched — no equivalent gap there.
- The `isGlobalTarget` fix (C) changes user-visible behavior: a saved BXT/
  SXT/NXT process icon double-click now always opens a dialog instead of
  running silently. This is the whole point of the fix (no more destructive
  silent runs, settings are now reviewable/editable) but is a deliberate
  behavior change worth flagging explicitly since it wasn't caught by any
  existing test (dialogs can't run under `--automation-mode`, so this path
  is unverifiable by the automated suite; verified only by code inspection
  against the `MaskMerge.js` convention).
- Fix H's `closeWindowById` unconditionally closes any existing same-named
  window with no confirmation prompt. This matches the fix's intent (a
  re-run is expected to replace, not accumulate) but means if a user
  independently renamed some *other* unrelated window to exactly
  `<id>_starless` it would be silently closed on the next SXT run against
  that source view. Considered acceptable: this id is SXT's own
  deterministic output-naming convention, not a generic user-facing name.
