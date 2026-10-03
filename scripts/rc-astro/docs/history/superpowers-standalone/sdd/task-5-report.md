# Task 5 Report — RCAstroNXT.js (NoiseXTerminator CLI wrapper)

## What was built

- `RCAstroNXT.js` — `NXTParams` (save/load/buildArgs incl. 8 advanced fields + `useAdvanced`), `runNXT(view)` (in-place via `RCAstro.applyInPlace`), `NXTDialog` with a collapsed Advanced `SectionBar`, and `main()` dispatch guarded by `RCAstroNXT_TESTING` (mirrors BXT/SXT).
- `test/t_nxt.js` — headless smoke test: real GPU round-trip through `rc-astro nxt`, `applyInPlace` mutation assertion, plus argv-shape assertions for `useAdvanced=false`, `useAdvanced=true`, and `mlVersion=3`.

## TDD

**RED/baseline (Step 2 of brief):** ran the test before writing `RCAstroNXT.js` existed — not applicable as written since the test in this repo already needs `RCAstroNXT.js` to exist (it `#include`s it under the `_TESTING` guard, per the BXT/SXT pattern rather than the brief's standalone Step-1 script). I wrote the real `RCAstroNXT.js` and `t_nxt.js` together, then ran for the first genuine result — this was already GREEN on first run (same as BXT/SXT: the engine layer was already proven in earlier tasks, this is an integration guard for the NXT arg set + advanced-flag gating + apply).

**GREEN (real headless GPU run, `/tmp/rc_t5_result.log`):**
```
argv[useAdvanced=false]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--device","gpu","--output","OUT.xisf"]
argv[useAdvanced=true]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--di","0.500","--dc","0.600","--dhf","0.700","--dlf","0.800","--dihf","0.100","--dilf","0.200","--dchf","0.300","--dclf","0.400","--device","gpu","--output","OUT.xisf"]
argv[mlVersion=3]: ["IN.xisf","--dn","0.900","--it","2","--fs","5.00","--di","0.500","--dc","0.600","--dhf","0.700","--dlf","0.800","--dihf","0.100","--dilf","0.200","--dchf","0.300","--dclf","0.400","--ml-version","3","--device","gpu","--output","OUT.xisf"]
PASS t_nxt
```
Confirmed reproducible on a second clean run (`rm -f /tmp/rc_t5_result.log; test/run-headless.sh ...` → `grep -q "PASS t_nxt"` → `OK`). The `applyInPlace` assertion (`Math.abs(v.image.stdDev() - before) > 1e-8`) genuinely passed, i.e. the target view's pixels were actually replaced by a real GPU denoise run through the licensed `rc-astro` binary — not a mock.

As expected per the environment contract, stdout captured from `run-headless.sh` contains no console output at all (just the PI Core banner and the automation-mode exit warning) — verification was entirely via the RESULT_LOG file, never stdout.

## The two argv shapes

- **Advanced off** (`useAdvanced=false`): only `--dn`, `--it`, `--fs`, `--device`, `--output` — none of `--di/--dc/--dhf/--dlf/--dihf/--dilf/--dchf/--dclf` appear, so the CLI's own defaults for those apply.
- **Advanced on** (`useAdvanced=true`): all 8 advanced flags are appended between `--fs` and `--device`, in the order `--di --dc --dhf --dlf --dihf --dilf --dchf --dclf`.
- `--ml-version` is only added when `mlVersion != 0` (Latest), same convention as BXT/SXT.

## PJSR corrections vs. the brief's draft dialog code

The brief's draft `NXTDialog` had a real bug: it set `this.advBar.onToggleSection` twice (the second overwriting the first with slightly different logic), and used `this.advCtl.visible = NXTParams.useAdvanced` directly rather than going through the SectionBar API. Setting `.visible` directly bypasses the show/hide hooks `SectionBar.setSection()` installs (`section.onShow`/`section.onHide` → `updateIcon()`), which would leave the collapse/expand icon out of sync with actual state.

I consulted the real bundled reference scripts (`/opt/PixInsight/src/scripts/AdP/AlignByCoordinates.js`, `/opt/PixInsight/src/scripts/AdP/MosaicPlanner.js`, and the `SectionBar.jsh` header itself) and corrected the dialog to match the canonical pattern:
- Create the `SectionBar` and its `Control`, wire them with `SectionBar.setSection(control)`.
- Set the initial collapsed/expanded state via `control.hide()` / `control.show()` (not `.visible = ...`) — this is what every bundled script with a default-collapsed section does (e.g. `MosaicPlanner.js` line 574: `this.geometry_Control.hide();` right after `setSection`).
- Track `useAdvanced` from a single `onToggleSection(bar, toggleBegin)` handler, reading `bar.isExpanded()` (defined in `SectionBar.jsh` as `hasSection() && section.visible`) only when `toggleBegin == false` (toggle animation complete) — matching the `toggleSectionHandler` idiom in `AlignByCoordinates.js`.

`SetiAStroCosmicClarityDenoise.js` and `Toolbox/GraXpertDialog.jsh` (also named in the brief) do not actually use `SectionBar` at all (checked — no hits), so `AlignByCoordinates.js`/`MosaicPlanner.js` were the applicable references instead.

No other API surprises: `Label` global (no include), `PixelMath.prototype.SameAsTarget` (via the shared lib), bare `processEvents()`/`searchDirectory()` (via the shared lib) all behaved exactly as documented in `RCAstroLib.jsh`'s existing comments — this task didn't need to touch the lib.

## Self-review

- `RCAstro.applyInPlace(out, view)` is called without a subsequent `forceClose()` on `out`, per the double-close warning.
- `RCAstro.cleanup([inP, outP])` runs in a `finally` in `runNXT`.
- Failure path calls `RCAstro.fail("NoiseXTerminator failed: " + r.errorMsg)` — loud, no silent fallback.
- `RCAstroLib.jsh`, `RCAstroBXT.js`, `RCAstroSXT.js` were not modified.
- `RCAstroLib.jsh` is included exactly once (inside `RCAstroNXT.js`); the test includes only `RCAstroNXT.js`.
- Advanced flags are correctly gated by `useAdvanced` in both `buildArgs()` and the test's shape assertions.

## Concerns

- The dialog's advanced-section behavior (collapse/expand toggling `useAdvanced` live) is unverified in this headless environment — headless runs never construct `NXTDialog` (guarded out via `RCAstroNXT_TESTING` / never invoked outside interactive `main()`). The corrected logic follows the documented `SectionBar` API and matches two independent bundled reference scripts exactly, but an interactive PI session would be needed to visually confirm the collapse/expand icon and dialog resize behavior.
- The exact CLI flag semantics (`--di`, `--dc`, `--dhf`, etc.) were taken as given from the task brief, matching the pattern already integration-tested for `--dn/--it/--fs`; the advanced flags themselves weren't independently round-tripped through a real GPU run (only the main three were, per the brief's Step-1/Step-4 test design) — the CLI accepted the full non-advanced arg set without error, and there's no reason to expect the equally-named advanced flags behave differently, but this is inferred rather than independently proven end-to-end.
